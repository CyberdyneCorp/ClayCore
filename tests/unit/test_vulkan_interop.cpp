#include <doctest/doctest.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"
#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// Device interop (evaluation-backends spec: a backend can be bound to a
// caller-supplied device; evaluation can write to device memory). Issue #43
// item 6: backends create and own their devices, so every brick and every mesh
// crosses host memory on its way from our GPU to the host's — even when, as on
// Linux since add-vulkan-backend, both sides are on the same physical device.
//
// The test creates a Vulkan device the way a HOST would, lends it to claycore,
// and checks the two things that matter:
//
//   1. A supplied device computes what an owned device computes. Adoption
//      changes where work runs, never what it computes.
//   2. Evaluating into a caller's buffer gives BIT-IDENTICAL floats to
//      evaluating into host memory — not "within tolerance". It is the same
//      shader on the same device; anything but equality is a plumbing bug,
//      which is exactly what this test is for.
//
// Skips when no Vulkan runtime is present, like the rest of the Vulkan suite.

using namespace clay;
using clay_test::item;

namespace {

// A device the TEST owns, standing in for the host's renderer. Deliberately
// created here rather than borrowed from the backend: adopting a device
// claycore made would not exercise the case this feature exists for.
struct HostDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    bool ok = false;

    HostDevice() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "claycore-host-test";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return;

        std::uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (n == 0) return;
        std::vector<VkPhysicalDevice> devices(n);
        vkEnumeratePhysicalDevices(instance, &n, devices.data());
        for (VkPhysicalDevice dev : devices) {
            std::uint32_t fn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &fn, nullptr);
            std::vector<VkQueueFamilyProperties> families(fn);
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &fn, families.data());
            for (std::uint32_t i = 0; i < fn; ++i)
                if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    physical = dev;
                    family = i;
                    break;
                }
            if (physical) break;
        }
        if (!physical) return;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) return;
        vkGetDeviceQueue(device, family, 0, &queue);
        ok = true;
    }

    ~HostDevice() {
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    HostDevice(const HostDevice&) = delete;
    HostDevice& operator=(const HostDevice&) = delete;

    eval::DeviceHandles handles() const {
        eval::DeviceHandles d;
        d.api = eval::DeviceApi::Vulkan;
        d.handles[0] = instance;
        d.handles[1] = physical;
        d.handles[2] = device;
        d.handles[3] = queue;
        d.queue_family = family;
        return d;
    }

    // A host-visible storage buffer the TEST owns, so the results can be read
    // back and compared without claycore touching them.
    struct Buf {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };

    Buf alloc(VkDeviceSize bytes) const {
        Buf b;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &b.buffer) != VK_SUCCESS) return b;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t index = UINT32_MAX;
        for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & want) == want) {
                index = i;
                break;
            }
        if (index == UINT32_MAX) return b;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = index;
        if (vkAllocateMemory(device, &mai, nullptr, &b.memory) != VK_SUCCESS) return b;
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        return b;
    }

    void free(Buf& b) const {
        if (b.mapped) vkUnmapMemory(device, b.memory);
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        b = Buf{};
    }
};

scene::Document scene_doc() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(1.0f), kernel::cf3(-0.3f, 0.0f, 0.0f)));
    l.sdf->insert(item(scene::Prim::box(kernel::cf3(0.6f, 0.5f, 0.4f)),
                       kernel::cf3(0.4f, 0.1f, 0.0f)));
    return doc;
}

eval::GridQuery grid_of(int n) {
    eval::GridQuery q;
    q.origin = kernel::cf3(-1.5f, -1.5f, -1.5f);
    q.spacing = 3.0f / static_cast<float>(n - 1);
    q.nx = q.ny = q.nz = n;
    return q;
}

}  // namespace

TEST_CASE("device interop: a supplied device computes what an owned device does") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Vulkan device; skipping");
        return;
    }
    std::unique_ptr<eval::Backend> adopted = eval::make_backend("vulkan", host.handles());
    if (!adopted) {
        MESSAGE("the vulkan backend declined to adopt; skipping");
        return;
    }
    CHECK(std::string(adopted->name()) == "vulkan");

    const scene::Document doc = scene_doc();
    const scene::Tape tape = scene::compile_document(doc);
    const eval::GridQuery q = grid_of(16);
    const std::size_t total = static_cast<std::size_t>(q.nx) * q.ny * q.nz;

    std::vector<float> from_adopted(total, 0.0f);
    REQUIRE(adopted->eval_grid(tape, q, from_adopted.data(), nullptr) == eval::Status::Ok);

    // Against the registered backend, which made its own device. Adoption
    // changes where work runs, never what it computes.
    if (eval::Backend* registered = eval::Registry::instance().find("vulkan")) {
        std::vector<float> from_registered(total, 0.0f);
        REQUIRE(registered->eval_grid(tape, q, from_registered.data(), nullptr) ==
                eval::Status::Ok);
        for (std::size_t i = 0; i < total; ++i) CHECK(from_adopted[i] == from_registered[i]);
    }

    // and against the scalar reference, so this is not two wrongs agreeing
    std::vector<float> points;
    points.reserve(total * 3);
    for (int k = 0; k < q.nz; ++k)
        for (int j = 0; j < q.ny; ++j)
            for (int i = 0; i < q.nx; ++i) {
                points.push_back(q.origin.x + static_cast<float>(i) * q.spacing);
                points.push_back(q.origin.y + static_cast<float>(j) * q.spacing);
                points.push_back(q.origin.z + static_cast<float>(k) * q.spacing);
            }
    std::vector<float> reference(total, 0.0f);
    eval::PointQuery pq{points.data(), total, 1e-4f};
    eval::PointResults pr{reference.data(), nullptr, nullptr};
    eval::eval_points_reference(tape, pq, pr);
    for (std::size_t i = 0; i < total; ++i)
        CHECK(from_adopted[i] == doctest::Approx(reference[i]).epsilon(1e-4));
}

TEST_CASE("device interop: results land in the caller's buffer, bit for bit") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Vulkan device; skipping");
        return;
    }
    std::unique_ptr<eval::Backend> adopted = eval::make_backend("vulkan", host.handles());
    if (!adopted) {
        MESSAGE("the vulkan backend declined to adopt; skipping");
        return;
    }

    const scene::Document doc = scene_doc();
    const scene::Tape tape = scene::compile_document(doc);
    const eval::GridQuery q = grid_of(16);
    const std::size_t total = static_cast<std::size_t>(q.nx) * q.ny * q.nz;
    const VkDeviceSize bytes = total * sizeof(float);

    HostDevice::Buf values = host.alloc(bytes);
    HostDevice::Buf colors = host.alloc(bytes * 3);
    REQUIRE(values.mapped != nullptr);
    REQUIRE(colors.mapped != nullptr);

    eval::DeviceBuffer dst{values.buffer, 0, bytes};
    eval::DeviceBuffer dst_colors{colors.buffer, 0, bytes * 3};
    REQUIRE(adopted->eval_grid_device(tape, q, dst, dst_colors) == eval::Status::Ok);

    std::vector<float> via_host(total, 0.0f), via_host_colors(total * 3, 0.0f);
    REQUIRE(adopted->eval_grid(tape, q, via_host.data(), via_host_colors.data()) ==
            eval::Status::Ok);

    // BIT-identical, not within a tolerance: same shader, same device, same
    // tape. Anything but equality is a bug in the plumbing.
    const float* got = static_cast<const float*>(values.mapped);
    const float* got_colors = static_cast<const float*>(colors.mapped);
    for (std::size_t i = 0; i < total; ++i) CHECK(got[i] == via_host[i]);
    for (std::size_t i = 0; i < total * 3; ++i) CHECK(got_colors[i] == via_host_colors[i]);

    SUBCASE("an offset into the caller's buffer is honoured") {
        // A host packs many bricks into one buffer, so writing at an offset is
        // the normal case rather than an edge one.
        const VkDeviceSize slot = 256 * sizeof(float);
        HostDevice::Buf packed = host.alloc(slot + bytes);
        REQUIRE(packed.mapped != nullptr);
        std::memset(packed.mapped, 0xCD, slot + bytes);
        eval::DeviceBuffer at_offset{packed.buffer, slot, bytes};
        REQUIRE(adopted->eval_grid_device(tape, q, at_offset, eval::DeviceBuffer{}) ==
                eval::Status::Ok);
        const auto* raw = static_cast<const unsigned char*>(packed.mapped);
        const float* shifted = reinterpret_cast<const float*>(raw + slot);
        for (std::size_t i = 0; i < total; ++i) CHECK(shifted[i] == via_host[i]);
        // and nothing before the offset was touched
        for (VkDeviceSize b = 0; b < slot; ++b) CHECK(raw[b] == 0xCD);
        host.free(packed);
    }

    SUBCASE("an undersized destination is refused with nothing written") {
        HostDevice::Buf small = host.alloc(bytes);
        REQUIRE(small.mapped != nullptr);
        std::memset(small.mapped, 0xCD, bytes);
        eval::DeviceBuffer short_dst{small.buffer, 0, bytes - 4};
        CHECK(adopted->eval_grid_device(tape, q, short_dst, eval::DeviceBuffer{}) ==
              eval::Status::InvalidInput);
        const auto* raw = static_cast<const unsigned char*>(small.mapped);
        for (VkDeviceSize b = 0; b < bytes; ++b) CHECK(raw[b] == 0xCD);
        // colours too: a colour buffer sized for distances is three times short
        eval::DeviceBuffer short_colors{small.buffer, 0, bytes};
        CHECK(adopted->eval_grid_device(tape, q, dst, short_colors) ==
              eval::Status::InvalidInput);
        host.free(small);
    }

    SUBCASE("a null destination is refused") {
        CHECK(adopted->eval_grid_device(tape, q, eval::DeviceBuffer{}, eval::DeviceBuffer{}) ==
              eval::Status::InvalidInput);
    }

    host.free(values);
    host.free(colors);
}

TEST_CASE("device interop: what cannot adopt says so") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Vulkan device; skipping");
        return;
    }
    const eval::DeviceHandles good = host.handles();

    // A backend with no adoption path. The caller falls back to the registry
    // and gets identical values, so this is a capability report, not a failure.
    CHECK(!eval::make_backend("cpu", good));
    CHECK(!eval::make_backend("no-such-backend", good));

    // A name and an API that do not agree.
    eval::DeviceHandles wrong_api = good;
    wrong_api.api = eval::DeviceApi::Metal;
    CHECK(!eval::make_backend("vulkan", wrong_api));

    // Incomplete handle sets, one missing handle at a time.
    for (int missing = 0; missing < 4; ++missing) {
        eval::DeviceHandles partial = good;
        partial.handles[missing] = nullptr;
        CAPTURE(missing);
        CHECK(!eval::make_backend("vulkan", partial));
    }

    // A queue family that does not exist, and so certainly does not compute.
    eval::DeviceHandles bad_family = good;
    bad_family.queue_family = 9999;
    CHECK(!eval::make_backend("vulkan", bad_family));

    // The registered backend is untouched by any of that.
    if (eval::Backend* registered = eval::Registry::instance().find("vulkan")) {
        const scene::Tape tape = scene::compile_document(scene_doc());
        std::vector<float> v(8 * 8 * 8, 0.0f);
        CHECK(registered->eval_grid(tape, grid_of(8), v.data(), nullptr) == eval::Status::Ok);
    }
}

// The registry's own backend made its own device, so a caller's VkBuffer means
// nothing to it. Reported rather than attempted: writing to a buffer from
// another device is undefined behaviour inside the driver.
TEST_CASE("device interop: the registered backend refuses a foreign buffer") {
    eval::Backend* registered = eval::Registry::instance().find("vulkan");
    if (!registered) {
        MESSAGE("no Vulkan backend registered; skipping");
        return;
    }
    const scene::Tape tape = scene::compile_document(scene_doc());
    eval::DeviceBuffer pretend{reinterpret_cast<void*>(0x1), 0, 1 << 20};
    CHECK(registered->eval_grid_device(tape, grid_of(8), pretend, eval::DeviceBuffer{}) ==
          eval::Status::Unsupported);
}

// -- across the C boundary ----------------------------------------------------

// The C ABI is the only surface a packaged consumer has, so the interop has to
// be usable from it — with no vendor header reaching clay.h, which is why the
// handles cross as void* under a named API rather than as VkDevice.
TEST_CASE("device interop: a host adopts its device through the C ABI") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Vulkan device; skipping");
        return;
    }
    clay_device_desc desc{};
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.api = CLAY_DEVICE_API_VULKAN;
    desc.handles[0] = host.instance;
    desc.handles[1] = host.physical;
    desc.handles[2] = host.device;
    desc.handles[3] = host.queue;
    desc.queue_family = host.family;

    clay_device* dev = clay_device_adopt(&desc);
    if (!dev) {
        MESSAGE("clay_device_adopt declined; skipping");
        return;
    }
    CHECK(std::string(clay_device_backend_name(dev)) == "vulkan");

    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    const float r = 0.5f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc, layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);

    constexpr int kN = 16;
    const std::size_t total = static_cast<std::size_t>(kN) * kN * kN;
    clay_grid_query grid{};
    grid.struct_size = static_cast<std::uint32_t>(sizeof grid);
    grid.origin[0] = grid.origin[1] = grid.origin[2] = -1.0f;
    grid.spacing = 2.0f / static_cast<float>(kN - 1);
    grid.dims[0] = grid.dims[1] = grid.dims[2] = kN;

    HostDevice::Buf buf = host.alloc(total * sizeof(float));
    REQUIRE(buf.mapped != nullptr);
    clay_device_buffer dst{};
    dst.struct_size = static_cast<std::uint32_t>(sizeof dst);
    dst.handle = buf.buffer;
    dst.offset = 0;
    dst.size = total * sizeof(float);

    REQUIRE(clay_eval_grid_device(doc, dev, &grid, nullptr, nullptr, &dst, nullptr) == CLAY_OK);

    // and it is the same field the host-memory call produces
    std::vector<float> host_side(total, 0.0f);
    REQUIRE(clay_eval_grid(doc, "cpu", &grid, nullptr, nullptr, host_side.data(), nullptr,
                           total) == CLAY_OK);
    const float* got = static_cast<const float*>(buf.mapped);
    for (std::size_t i = 0; i < total; ++i)
        CHECK(got[i] == doctest::Approx(host_side[i]).epsilon(1e-4));

    SUBCASE("every refusal") {
        clay_device_buffer small = dst;
        small.size = total * sizeof(float) - 4;
        CHECK(clay_eval_grid_device(doc, dev, &grid, nullptr, nullptr, &small, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_device_buffer no_handle = dst;
        no_handle.handle = nullptr;
        CHECK(clay_eval_grid_device(doc, dev, &grid, nullptr, nullptr, &no_handle, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_device_buffer no_size = dst;
        no_size.size = 0;
        CHECK(clay_eval_grid_device(doc, dev, &grid, nullptr, nullptr, &no_size, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_eval_grid_device(nullptr, dev, &grid, nullptr, nullptr, &dst, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_eval_grid_device(doc, nullptr, &grid, nullptr, nullptr, &dst, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_eval_grid_device(doc, dev, &grid, nullptr, nullptr, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // one cull bound without the other, as clay_eval_grid refuses it
        const float lo[3] = {-1, -1, -1};
        CHECK(clay_eval_grid_device(doc, dev, &grid, lo, nullptr, &dst, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a cull region works, as it does on the host path") {
        const float lo[3] = {-1.2f, -1.2f, -1.2f}, hi[3] = {1.2f, 1.2f, 1.2f};
        REQUIRE(clay_eval_grid_device(doc, dev, &grid, lo, hi, &dst, nullptr) == CLAY_OK);
    }

    SUBCASE("a whole drain lands in one buffer at a fixed stride") {
        // The call ClaySpaceDesktop actually wants: every dirty brick
        // evaluated straight into the allocation the host will draw from.
        clay_brick_config cfg;
        cfg.struct_size = sizeof(cfg);
        REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
        clay_brick_cache* cache = clay_brick_cache_create(&cfg);
        REQUIRE(cache != nullptr);
        REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc, layer) == CLAY_OK);
        constexpr std::size_t kPer = 8 * 8 * 8;
        constexpr std::size_t kChunk = 8;
        std::vector<clay_brick_request> reqs(kChunk);
        std::size_t n = kChunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &n, &remaining) == CLAY_OK);
        REQUIRE(n > 0);

        HostDevice::Buf bricks = host.alloc(n * kPer * sizeof(float));
        REQUIRE(bricks.mapped != nullptr);
        clay_device_buffer brick_dst{};
        brick_dst.struct_size = static_cast<std::uint32_t>(sizeof brick_dst);
        brick_dst.handle = bricks.buffer;
        brick_dst.size = n * kPer * sizeof(float);
        REQUIRE(clay_brick_cache_eval_requests_device(doc, dev, reqs.data(), n, &brick_dst,
                                                      nullptr) == CLAY_OK);

        // Every brick matches the host-memory path at its own stride.
        std::vector<float> host_bricks(n * kPer, 0.0f);
        REQUIRE(clay_brick_cache_eval_requests(doc, "cpu", reqs.data(), n, host_bricks.data(),
                                               n * kPer, nullptr, 0) == CLAY_OK);
        const float* got_bricks = static_cast<const float*>(bricks.mapped);
        for (std::size_t i = 0; i < n * kPer; ++i)
            CHECK(got_bricks[i] == doctest::Approx(host_bricks[i]).epsilon(1e-4));

        // and a buffer that cannot hold the whole batch is refused BEFORE the
        // bricks that would fit have already landed
        std::vector<float> before(n * kPer);
        std::memcpy(before.data(), bricks.mapped, n * kPer * sizeof(float));
        clay_device_buffer too_small = brick_dst;
        too_small.size = (n - 1) * kPer * sizeof(float);
        CHECK(clay_brick_cache_eval_requests_device(doc, dev, reqs.data(), n, &too_small,
                                                    nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(std::memcmp(before.data(), bricks.mapped, n * kPer * sizeof(float)) == 0);

        host.free(bricks);
        clay_brick_cache_destroy(cache);
    }

    SUBCASE("an unbuilt API is refused at adoption, with the reason available") {
        clay_device_desc metal = desc;
        metal.api = CLAY_DEVICE_API_METAL;
        CHECK(clay_device_adopt(&metal) == nullptr);
        CHECK(std::string(clay_last_error()).find("adopt") != std::string::npos);
        clay_device_desc unknown = desc;
        unknown.api = 99;
        CHECK(clay_device_adopt(&unknown) == nullptr);
        clay_device_desc stunted = desc;
        stunted.struct_size = 4;
        CHECK(clay_device_adopt(&stunted) == nullptr);
    }

    host.free(buf);
    clay_document_destroy(doc);
    clay_device_release(dev);
    clay_device_release(nullptr);  // releasing a null handle is a no-op
}

// -- the resumable refill on a device destination (#345) ---------------------
//
// The host-memory refill has kept each brick's float32 result as a seed since
// #306; its device-buffer sibling did none of that, so the callers most likely
// to be latency-bound -- a renderer evaluating into the buffer it will draw
// from -- walked the whole surviving edit list over every sample, every dab.
//
// What this pins is not the speed, which is the benchmark harness's, but the
// two things resuming must not bend:
//
//   1. A resumed brick holds what a FULL WALK of the same document holds.
//      The oracle is a document rebuilt from the same items and refilled
//      through the CPU with no seed to its name, so it is a full walk by
//      construction and not the host resumed path checking itself.
//   2. The resumed path actually FIRES. It is bit-identical to the full walk
//      by contract, so nothing in the output can say whether it ran, and a
//      fast path that quietly stopped firing reads as correct --
//      clay_document_resume_stats is the only witness.
namespace {

// The parity suite's relative error (test_parity.cpp), so the bar this holds
// the device path to is the bar every backend is already held to.
float rel_err(float a, float b) {
    const float scale = std::max(std::max(std::fabs(a), std::fabs(b)), 1.0f);
    return std::fabs(a - b) / scale;
}

// A dab, kept so the oracle document can be rebuilt item for item.
struct Dab {
    float radius;
    float pos[3];
};

clay_document* build(const std::vector<Dab>& dabs, clay_layer_id* out_layer) {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    for (const Dab& d : dabs) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &d.radius, 1);
        REQUIRE(it != nullptr);
        REQUIRE(clay_item_set_position(it, d.pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
    }
    if (out_layer) *out_layer = layer;
    return doc;
}

// A window of bricks over the sphere's equator, where every brick either
// straddles the surface or lies inside it. A window that runs off the shape
// holds bricks whose culled prefix produced nothing at all, which are
// correctly walked in full for ever -- a legitimate refusal that would sit in
// the counter below pretending to be the defect it is gating.
std::vector<clay_brick_request> equator_window(int n) {
    std::vector<clay_brick_request> reqs(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        std::memset(&reqs[static_cast<std::size_t>(i)], 0, sizeof(clay_brick_request));
        clay_brick_request& r = reqs[static_cast<std::size_t>(i)];
        r.key[0] = i - 1;
        r.key[1] = 0;
        r.key[2] = 0;
        for (int a = 0; a < 3; ++a)
            r.origin[a] = static_cast<float>(r.key[a]) * 8 * 0.05f;
        r.spacing = 0.05f;
        r.dims[0] = r.dims[1] = r.dims[2] = 8;
        r.band = 0.15f;
    }
    return reqs;
}

}  // namespace

TEST_CASE("device refill: a stroke resumes into the caller's buffer and matches a full walk") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Vulkan device; skipping");
        return;
    }
    clay_device_desc desc{};
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.api = CLAY_DEVICE_API_VULKAN;
    desc.handles[0] = host.instance;
    desc.handles[1] = host.physical;
    desc.handles[2] = host.device;
    desc.handles[3] = host.queue;
    desc.queue_family = host.family;
    clay_device* dev = clay_device_adopt(&desc);
    REQUIRE(dev != nullptr);

    // A sculpt with enough history that a full walk and a suffix are visibly
    // different amounts of work, then a stroke of dabs on top of it.
    std::vector<Dab> dabs;
    dabs.push_back(Dab{1.0f, {0.0f, 0.0f, 0.0f}});
    for (int i = 0; i < 200; ++i) {
        const float t = static_cast<float>(i) * 0.031f;
        dabs.push_back(Dab{0.08f, {std::cos(t), std::sin(t) * 0.4f, std::sin(t * 1.7f) * 0.4f}});
    }

    clay_layer_id layer = 0;
    clay_document* doc = build(dabs, &layer);
    const std::vector<clay_brick_request> reqs = equator_window(6);
    const std::size_t count = reqs.size();
    const std::size_t per = 8 * 8 * 8;
    const std::size_t total = count * per;

    HostDevice::Buf values = host.alloc(total * sizeof(float));
    HostDevice::Buf colors = host.alloc(total * 3 * sizeof(float));
    REQUIRE(values.mapped != nullptr);
    REQUIRE(colors.mapped != nullptr);
    clay_device_buffer dst{};
    dst.struct_size = static_cast<std::uint32_t>(sizeof dst);
    dst.handle = values.buffer;
    dst.size = total * sizeof(float);
    clay_device_buffer dst_colors = dst;
    dst_colors.handle = colors.buffer;
    dst_colors.size = total * 3 * sizeof(float);

    // Primed in the MIDDLE, and through the host-memory entry point, so the
    // first device call has to hold three cases apart at once: a run of
    // resumable bricks that does NOT start at brick 0, and un-resumable bricks
    // on both sides of it. Every destination here is a fixed slot, so a run
    // that lands at the run's own offset and a run that lands at the buffer's
    // start differ only when the run does not start at 0 -- which is the shape
    // a moving dirty window is in every dab but the first. The seed store is
    // the DOCUMENT's, not either entry point's, so priming through the host
    // path is also the check that one refill's seeds serve the other's.
    std::vector<float> prime(4 * per), prime_rgb(4 * per * 3);
    REQUIRE(clay_brick_cache_eval_requests(doc, "cpu", reqs.data() + 1, 4, prime.data(),
                                           prime.size(), prime_rgb.data(),
                                           prime_rgb.size()) == CLAY_OK);

    std::vector<float> oracle(total), oracle_rgb(total * 3);
    float worst = 0.0f, worst_rgb = 0.0f;
    for (int step = 0; step < 6; ++step) {
        // One dab, appended -- which is the case a suffix exists for.
        const float t = static_cast<float>(step) * 0.05f;
        dabs.push_back(Dab{0.07f, {0.95f, t, -0.1f}});
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &dabs.back().radius, 1);
        REQUIRE(it != nullptr);
        REQUIRE(clay_item_set_position(it, dabs.back().pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);

        REQUIRE(clay_brick_cache_eval_requests_device(doc, dev, reqs.data(), count, &dst,
                                                      &dst_colors) == CLAY_OK);

        // The oracle: the same items in a document that has never been
        // refilled, so its CPU refill walks the whole edit list.
        clay_document* fresh = build(dabs, nullptr);
        REQUIRE(clay_brick_cache_eval_requests(fresh, "cpu", reqs.data(), count, oracle.data(),
                                               total, oracle_rgb.data(), total * 3) == CLAY_OK);
        clay_document_destroy(fresh);

        const float* got = static_cast<const float*>(values.mapped);
        const float* got_rgb = static_cast<const float*>(colors.mapped);
        for (std::size_t i = 0; i < total; ++i) worst = std::max(worst, rel_err(got[i], oracle[i]));
        for (std::size_t i = 0; i < total * 3; ++i)
            worst_rgb = std::max(worst_rgb, rel_err(got_rgb[i], oracle_rgb[i]));
    }
    // The parity suite's standard for a GPU backend against the CPU scalar
    // reference (test_parity.cpp): 1e-4 relative. Not bit-identity, because the
    // half of each answer that was NOT resumed came off the GPU and the oracle
    // is the CPU; identity is the wrong bar for that pair, and it is the bar
    // the device path already had to meet before it resumed anything.
    CHECK(worst <= 1e-4f);
    CHECK(worst_rgb <= 1e-4f);

    // And it fired. Without this the two checks above pass just as happily on
    // a device path that resumes nothing, which is what they did before #345.
    clay_resume_stats rs{};
    rs.struct_size = static_cast<std::uint32_t>(sizeof rs);
    REQUIRE(clay_document_resume_stats(doc, &rs) == CLAY_OK);
    CHECK(rs.resumed_bricks > 0);
    // The bricks outside the primed middle were walked in full on the first
    // device call, which is what left them a seed.
    CHECK(rs.refilled_bricks >= 2);
    // and carried the stroke rather than firing once. Not "every brick after
    // the first call": the cull pad GROWS as a stroke reaches outward, and a
    // seed taken under a smaller one is correctly refused and refilled, which
    // is a legitimate refusal the host path makes too.
    CHECK(rs.resumed_bricks > rs.refilled_bricks);
    CHECK(rs.entries >= count);

    host.free(values);
    host.free(colors);
    clay_document_destroy(doc);
    clay_device_release(dev);
}
