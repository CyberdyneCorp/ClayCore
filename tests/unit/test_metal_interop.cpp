#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "clay.h"
#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// Metal device interop (evaluation-backends spec: batched grid evaluation
// amortizes device dispatch — the device-buffer form). The zero-copy refill
// (clay_brick_cache_eval_requests_device) is the call an iPad renderer makes
// every frame, and until the batched device form existed it dispatched one
// command buffer per brick — 25-165x behind the host-memory route on the
// same GPU. Speed is the benchmark harness's to hold; what this suite pins
// is the CONTRACT the batching must not bend:
//
//   1. The batched device form computes exactly what the host-memory batch
//      computes, BIT-identical — same backend, same device, same kernel;
//      anything but equality is a plumbing bug.
//   2. Brick i lands at the documented fixed slot, a caller's base offset is
//      honoured, and bytes outside the slots are never touched.
//   3. The C ABI's device path agrees with the host-memory path a renderer
//      would otherwise use, over a real cache's requests — far-empty bricks
//      (empty culled tapes, the zero-length slice in the concatenated
//      upload) included.
//
// The test creates a Metal device the way a HOST would and lends it to
// claycore; adopting a device claycore made would not exercise the case this
// feature exists for. Skips when no Metal device is present.

using namespace clay;
using clay_test::item;

namespace {

// A device the TEST owns, standing in for the host's renderer.
struct HostDevice {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
    bool ok = false;

    HostDevice() {
        device = MTL::CreateSystemDefaultDevice();
        if (!device) return;
        queue = device->newCommandQueue();
        ok = queue != nullptr;
    }
    ~HostDevice() {
        if (queue) queue->release();
        if (device) device->release();
    }
    HostDevice(const HostDevice&) = delete;
    HostDevice& operator=(const HostDevice&) = delete;

    eval::DeviceHandles handles() const {
        eval::DeviceHandles d;
        d.api = eval::DeviceApi::Metal;
        d.handles[0] = device;
        d.handles[1] = queue;
        return d;
    }

    MTL::Buffer* alloc(std::size_t bytes) const {
        return device->newBuffer(bytes, MTL::ResourceStorageModeShared);
    }
};

scene::Document scene_doc() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.4f), kernel::cf3(-0.2f, 0.0f, 0.0f)));
    l.sdf->insert(item(scene::Prim::box(kernel::cf3(0.3f, 0.25f, 0.2f)),
                       kernel::cf3(0.3f, 0.1f, 0.0f)));
    return doc;
}

}  // namespace

TEST_CASE("metal interop: the batched device form is the host-memory batch, bit for bit") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Metal device; skipping");
        return;
    }
    std::unique_ptr<eval::Backend> adopted = eval::make_backend("metal", host.handles());
    REQUIRE(adopted != nullptr);

    const scene::Document doc = scene_doc();
    // Per-brick culled tapes, the refill shape — including one culled to a
    // region far from every item, whose tape is EMPTY: the zero-length slice
    // in the concatenated upload.
    const float spacing = 0.05f;
    const int n = 8;
    const std::size_t per = static_cast<std::size_t>(n) * n * n;
    std::vector<kernel::cfloat3> origins = {
        kernel::cf3(-0.4f, -0.2f, -0.2f), kernel::cf3(0.0f, -0.2f, -0.2f),
        kernel::cf3(0.2f, 0.0f, -0.1f),   kernel::cf3(5.0f, 5.0f, 5.0f),
        kernel::cf3(-0.2f, 0.0f, 0.0f),
    };
    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> tape_ptrs;
    tapes.reserve(origins.size());  // the pointers below must survive growth
    for (const kernel::cfloat3& o : origins) {
        const float side = spacing * static_cast<float>(n);
        scene::CullRegion cull{math::Aabb{o, o + kernel::cf3(side, side, side)}.dilated(0.15f)};
        tapes.push_back(scene::compile_document(doc, &cull));
        tape_ptrs.push_back(&tapes.back());
    }
    CHECK(tapes[3].instrs.empty());  // the far brick really is the empty-tape case

    eval::GridBatchQuery q;
    q.tapes = tape_ptrs.data();
    q.origins = origins.data();
    q.spacing = spacing;
    q.nx = q.ny = q.nz = n;
    q.count = origins.size();
    const std::size_t total = per * q.count;

    // The reference: the host-memory batch through the SAME adopted backend.
    std::vector<float> host_values(total), host_colors(total * 3);
    REQUIRE(adopted->eval_grid_batch(q, host_values.data(), host_colors.data()) ==
            eval::Status::Ok);

    // The caller packs results into a larger allocation at an offset — the
    // normal case for an atlas, not an edge one.
    const std::size_t base = 64 * sizeof(float);
    MTL::Buffer* values = host.alloc(base + total * sizeof(float));
    MTL::Buffer* colors = host.alloc(base + total * 3 * sizeof(float));
    REQUIRE(values != nullptr);
    REQUIRE(colors != nullptr);
    std::memset(values->contents(), 0xCD, base + total * sizeof(float));
    std::memset(colors->contents(), 0xCD, base + total * 3 * sizeof(float));

    eval::DeviceBuffer dst{values, base, total * sizeof(float)};
    eval::DeviceBuffer dst_colors{colors, base, total * 3 * sizeof(float)};
    REQUIRE(adopted->eval_grid_batch_device(q, dst, dst_colors) == eval::Status::Ok);

    const auto* raw = static_cast<const unsigned char*>(values->contents());
    const auto* raw_colors = static_cast<const unsigned char*>(colors->contents());
    const float* got = reinterpret_cast<const float*>(raw + base);
    const float* got_colors = reinterpret_cast<const float*>(raw_colors + base);
    for (std::size_t i = 0; i < total; ++i) CHECK(got[i] == host_values[i]);
    for (std::size_t i = 0; i < total * 3; ++i) CHECK(got_colors[i] == host_colors[i]);
    // and nothing before the offset was touched
    for (std::size_t b = 0; b < base; ++b) {
        CHECK(raw[b] == 0xCD);
        CHECK(raw_colors[b] == 0xCD);
    }

    SUBCASE("an undersized destination is refused with nothing written") {
        MTL::Buffer* small = host.alloc(total * sizeof(float));
        REQUIRE(small != nullptr);
        std::memset(small->contents(), 0xCD, total * sizeof(float));
        eval::DeviceBuffer short_dst{small, 0, total * sizeof(float) - 4};
        CHECK(adopted->eval_grid_batch_device(q, short_dst, eval::DeviceBuffer{}) ==
              eval::Status::InvalidInput);
        const auto* bytes = static_cast<const unsigned char*>(small->contents());
        for (std::size_t b = 0; b < total * sizeof(float); ++b) CHECK(bytes[b] == 0xCD);
        // colours too: a colour buffer sized for distances is three times short
        eval::DeviceBuffer short_colors{small, 0, total * sizeof(float)};
        CHECK(adopted->eval_grid_batch_device(q, dst, short_colors) ==
              eval::Status::InvalidInput);
        small->release();
    }

    SUBCASE("an empty batch is Ok") {
        eval::GridBatchQuery empty = q;
        empty.count = 0;
        CHECK(adopted->eval_grid_batch_device(empty, dst, eval::DeviceBuffer{}) ==
              eval::Status::Ok);
    }

    SUBCASE("a backend that owns its device refuses: an MTLBuffer belongs to its maker") {
        if (eval::Backend* owned = eval::Registry::instance().find("metal"))
            CHECK(owned->eval_grid_batch_device(q, dst, eval::DeviceBuffer{}) ==
                  eval::Status::Unsupported);
    }

    values->release();
    colors->release();
}

TEST_CASE("metal interop: the C ABI device refill agrees with the host-memory refill") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Metal device; skipping");
        return;
    }

    clay_device_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.api = CLAY_DEVICE_API_METAL;
    desc.handles[0] = host.device;
    desc.handles[1] = host.queue;
    clay_device* cdev = clay_device_adopt(&desc);
    REQUIRE(cdev != nullptr);
    REQUIRE(std::string(clay_device_backend_name(cdev)) == "metal");

    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    for (int i = 0; i < 3; ++i) {
        float r = 0.25f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        const float pos[3] = {0.2f * static_cast<float>(i), 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
    }

    clay_brick_config cfg;
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.dim = 8;
    cfg.voxel_size = 0.05f;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);
    // Wider than the geometry, so the batch spans surface, inside, outside
    // AND far-empty bricks — the empty culled tape rides in the same batch.
    const float lo[3] = {-0.6f, -0.6f, -0.6f}, hi[3] = {1.2f, 0.6f, 0.6f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);

    std::vector<clay_brick_request> requests(4096);
    std::size_t count = requests.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, requests.data(), &count, &remaining) == CLAY_OK);
    REQUIRE(remaining == 0);
    REQUIRE(count > 30);  // enough bricks that the batch is a real batch
    const std::size_t per = 8 * 8 * 8;
    const std::size_t total = count * per;

    // The reference: the registered "metal" backend's host-memory refill —
    // the route a renderer takes today, and the values the zero-copy path
    // promises to reproduce exactly.
    std::vector<float> host_values(total), host_colors(total * 3);
    REQUIRE(clay_brick_cache_eval_requests(doc, "metal", requests.data(), count,
                                           host_values.data(), total, host_colors.data(),
                                           total * 3) == CLAY_OK);

    MTL::Buffer* values = host.alloc(total * sizeof(float));
    MTL::Buffer* colors = host.alloc(total * 3 * sizeof(float));
    REQUIRE(values != nullptr);
    REQUIRE(colors != nullptr);

    clay_device_buffer dst;
    std::memset(&dst, 0, sizeof dst);
    dst.struct_size = sizeof dst;
    dst.handle = values;
    dst.offset = 0;
    dst.size = total * sizeof(float);
    clay_device_buffer dst_colors = dst;
    dst_colors.handle = colors;
    dst_colors.size = total * 3 * sizeof(float);

    REQUIRE(clay_brick_cache_eval_requests_device(doc, cdev, requests.data(), count, &dst,
                                                  &dst_colors) == CLAY_OK);

    const float* got = static_cast<const float*>(values->contents());
    const float* got_colors = static_cast<const float*>(colors->contents());
    std::size_t value_diffs = 0, color_diffs = 0;
    for (std::size_t i = 0; i < total; ++i) value_diffs += got[i] != host_values[i];
    for (std::size_t i = 0; i < total * 3; ++i) color_diffs += got_colors[i] != host_colors[i];
    CHECK(value_diffs == 0);
    CHECK(color_diffs == 0);

    values->release();
    colors->release();
    clay_brick_cache_destroy(cache);
    clay_document_destroy(doc);
    clay_device_release(cdev);
}
