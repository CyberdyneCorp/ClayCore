// Vulkan compute backend (evaluation-backends spec, tier 3).
//
// Shaders are SPIR-V compiled at build time from the amalgamated kernel
// headers (tools/amalgamate_glsl.py + glslang), so the math is the same
// single source every other backend compiles. Scope is eval_points and
// eval_grid (the brick-fill primitive); raycast and device meshing report
// Unsupported and fall back to CPU -- the tier-3 contract is that a missing
// capability never changes results, only speed.
//
// Two things are deliberate and differ from the older GPU backends:
//
//   * The tape is uploaded when it CHANGES, not on every dispatch. A brush
//     dab evaluates ~24 bricks against one document, and re-uploading the
//     whole tape 24 times is work whose result is already on the device.
//     Residency is keyed on an exact content compare rather than a hash,
//     because a key that can collide is a wrong field rather than a slow one.
//   * Buffers are reused and grown, not allocated and freed per call.
//
// Requires Vulkan 1.1 and nothing else: no buffer_device_address, no
// descriptor indexing, no 16-bit storage. Cursors into the tape's float pool
// are indices into a storage buffer (see the shim's CLAY_AT), which is what
// keeps the floor that low.

#include <vulkan/vulkan.h>

#include <cstring>
#include <vector>

#include "clay/eval/backend.h"

#include "vulkan_dispatch.h"

extern "C" {
extern const unsigned char clay_spv_points[];
extern const unsigned int clay_spv_points_size;
extern const unsigned char clay_spv_grid[];
extern const unsigned int clay_spv_grid_size;
extern const unsigned char clay_spv_copy[];
extern const unsigned int clay_spv_copy_size;
}

namespace clay {
namespace eval {

namespace {

constexpr std::uint32_t kLocalSize = 64;  // must match the shader's local_size_x
constexpr std::uint32_t kBindings = 5;

// Mirrors the shader's push-constant block exactly. Every member is four
// bytes, so there is no padding to disagree about.
struct PushConstants {
    float origin_x = 0.0f, origin_y = 0.0f, origin_z = 0.0f;
    float spacing = 0.0f;
    std::uint32_t instr_count = 0;
    std::uint32_t params_base = 0;
    std::uint32_t blob_base = 0;
    std::uint32_t count = 0;
    std::uint32_t nx = 0, ny = 0, nz = 0;
    std::uint32_t has_colors = 0;
    std::uint32_t base = 0;
};
static_assert(sizeof(PushConstants) == 13 * 4, "push constants must stay flat");

// A host-visible storage buffer that grows in place. Host-visible rather
// than device-local everywhere: these are tape-sized and result-sized
// buffers written and read once per dispatch, so a staging copy would add a
// transfer and a barrier to save nothing.
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

class VulkanBackend final : public Backend {
  public:
    static std::unique_ptr<VulkanBackend> create() {
        auto b = std::unique_ptr<VulkanBackend>(new VulkanBackend());
        return b->init() ? std::move(b) : nullptr;
    }

    // Bound to a device the CALLER owns. Everything above the device — the
    // pipelines, the descriptor pool, the command buffer, the staging buffers —
    // is still ours and is still built and destroyed here; only the device,
    // queue and instance are borrowed.
    static std::unique_ptr<VulkanBackend> adopt(const DeviceHandles& d) {
        auto b = std::unique_ptr<VulkanBackend>(new VulkanBackend());
        return b->adopt_init(d) ? std::move(b) : nullptr;
    }

    ~VulkanBackend() override {
        if (device_) {
            vkDeviceWaitIdle(device_);
            for (Buffer* b : {&instrs_, &floats_, &in_, &dist_, &color_}) destroy(*b);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
            if (pipe_points_) vkDestroyPipeline(device_, pipe_points_, nullptr);
            if (pipe_grid_) vkDestroyPipeline(device_, pipe_grid_, nullptr);
            if (pipe_copy_) vkDestroyPipeline(device_, pipe_copy_, nullptr);
            if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
            if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
            // Destroy nothing we did not make. On an adopted device the caller
            // keeps its device and instance, and destroying them here would
            // take down the host's renderer along with our backend.
            if (owns_device_) vkDestroyDevice(device_, nullptr);
        }
        if (owns_device_ && instance_) vkDestroyInstance(instance_, nullptr);
    }

    const char* name() const override { return "vulkan"; }
    std::uint64_t tape_uploads() const { return tape_uploads_; }
    std::uint64_t tape_patches() const { return tape_patches_; }
    BackendCaps caps() const override {
        BackendCaps c{false, false, 0};
        // Only on an ADOPTED device: `device_copy` names a buffer the CALLER
        // lent us, and one from the device we made for ourselves is not that.
        c.device_copy = !owns_device_;
        return c;
    }

    Status eval_points(const scene::Tape& tape, const PointQuery& q,
                       const PointResults& out) override {
        if (!q.points_xyz || !out.distances) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;

        PushConstants pc{};
        pc.count = static_cast<std::uint32_t>(q.count);
        pc.has_colors = out.colors_rgb ? 1u : 0u;
        if (!upload_tape(tape, &pc)) return Status::DeviceError;
        if (!ensure(&in_, q.count * 3 * sizeof(float))) return Status::DeviceError;
        if (!ensure(&dist_, q.count * sizeof(float))) return Status::DeviceError;
        if (!ensure(&color_, (out.colors_rgb ? q.count * 3 : 1) * sizeof(float)))
            return Status::DeviceError;
        std::memcpy(in_.mapped, q.points_xyz, q.count * 3 * sizeof(float));

        if (!dispatch(pipe_points_, pc, q.count)) return Status::DeviceError;

        std::memcpy(out.distances, dist_.mapped, q.count * sizeof(float));
        if (out.colors_rgb)
            std::memcpy(out.colors_rgb, color_.mapped, q.count * 3 * sizeof(float));
        // Gradients stay on the host, as they do on the OpenCL backend: the
        // tetrahedron tap lives in field.h, which is templated C++ and not
        // part of any compute dialect. Stated rather than silent -- a caller
        // asking a tier-3 backend for gradients is paying CPU for them.
        if (out.gradients_xyz) {
            PointResults grad_only;
            grad_only.gradients_xyz = out.gradients_xyz;
            std::vector<float> scratch(q.count);
            grad_only.distances = scratch.data();
            eval_points_reference(tape, q, grad_only);
        }
        return Status::Ok;
    }

    Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                     float* out_colors_rgb) override {
        if (!out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        const std::size_t total =
            static_cast<std::size_t>(q.nx) * static_cast<std::size_t>(q.ny) *
            static_cast<std::size_t>(q.nz);

        PushConstants pc{};
        pc.origin_x = q.origin.x;
        pc.origin_y = q.origin.y;
        pc.origin_z = q.origin.z;
        pc.spacing = q.spacing;
        pc.nx = static_cast<std::uint32_t>(q.nx);
        pc.ny = static_cast<std::uint32_t>(q.ny);
        pc.nz = static_cast<std::uint32_t>(q.nz);
        pc.count = static_cast<std::uint32_t>(total);
        pc.has_colors = out_colors_rgb ? 1u : 0u;
        if (!upload_tape(tape, &pc)) return Status::DeviceError;
        if (!ensure(&in_, sizeof(float))) return Status::DeviceError;  // unused, must bind
        if (!ensure(&dist_, total * sizeof(float))) return Status::DeviceError;
        if (!ensure(&color_, (out_colors_rgb ? total * 3 : 1) * sizeof(float)))
            return Status::DeviceError;

        if (!dispatch(pipe_grid_, pc, total)) return Status::DeviceError;

        std::memcpy(out_values, dist_.mapped, total * sizeof(float));
        if (out_colors_rgb)
            std::memcpy(out_colors_rgb, color_.mapped, total * 3 * sizeof(float));
        return Status::Ok;
    }

    // The point of adoption: results land in the caller's own buffer, so a host
    // that was going to draw them never copies them through host memory.
    //
    // The same shader, the same tape, the same push constants as eval_grid —
    // only the destination differs. That is deliberate and is what the test
    // rests on: the two paths must agree BIT for bit, not within a tolerance.
    Status eval_grid_device(const scene::Tape& tape, const GridQuery& q,
                            const DeviceBuffer& values,
                            const DeviceBuffer& colors) override {
        // Only an ADOPTED backend can serve this: a VkBuffer belongs to the
        // device that made it, and one from the caller's device is meaningless
        // on the device we created for ourselves.
        if (owns_device_) return Status::Unsupported;
        if (values.empty() || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        const std::size_t total = static_cast<std::size_t>(q.nx) *
                                  static_cast<std::size_t>(q.ny) *
                                  static_cast<std::size_t>(q.nz);
        // Checked against the lattice BEFORE any dispatch, so a destination too
        // small is refused with nothing written rather than partially filled.
        if (values.size < static_cast<std::uint64_t>(total) * sizeof(float))
            return Status::InvalidInput;
        const bool want_colors = !colors.empty();
        if (want_colors && colors.size < static_cast<std::uint64_t>(total) * 3 * sizeof(float))
            return Status::InvalidInput;

        PushConstants pc{};
        pc.origin_x = q.origin.x;
        pc.origin_y = q.origin.y;
        pc.origin_z = q.origin.z;
        pc.spacing = q.spacing;
        pc.nx = static_cast<std::uint32_t>(q.nx);
        pc.ny = static_cast<std::uint32_t>(q.ny);
        pc.nz = static_cast<std::uint32_t>(q.nz);
        pc.count = static_cast<std::uint32_t>(total);
        pc.has_colors = want_colors ? 1u : 0u;
        if (!upload_tape(tape, &pc)) return Status::DeviceError;
        if (!ensure(&in_, sizeof(float))) return Status::DeviceError;  // unused, must bind
        // Binding 4 must be a real buffer even when no colours were asked for.
        if (!want_colors && !ensure(&color_, sizeof(float))) return Status::DeviceError;

        const Borrowed dist{static_cast<VkBuffer>(values.handle), values.offset, values.size};
        const Borrowed color{static_cast<VkBuffer>(colors.handle), colors.offset, colors.size};
        if (!dispatch(pipe_grid_, pc, total, &dist, want_colors ? &color : nullptr))
            return Status::DeviceError;
        // dispatch() waits on its own fence before returning, so the work has
        // COMPLETED here — nothing is left in flight on the caller's queue.
        return Status::Ok;
    }

    // -- host memory <-> a buffer the caller owns (#345) ---------------------
    //
    // What these carry is the resumable refill's traffic across the device
    // boundary: a brick answered from its seed is computed on the host and
    // WRITTEN into the caller's slot, and a brick that had to be walked in full
    // is READ back so it becomes the next dab's seed. Both are a few kilobytes
    // a brick, against a full walk of the surviving edit list per sample.
    //
    // Both run the copy shader over our own host-visible staging buffer, so the
    // caller's buffer is touched only through the storage binding the
    // evaluation path already binds it to. clay_kernels.comp.in says why that
    // is not vkCmdCopyBuffer.
    Status write_device_buffer(const DeviceBuffer& dst, const void* src,
                               std::uint64_t bytes) override {
        if (owns_device_) return Status::Unsupported;
        if (dst.empty() || !src) return Status::InvalidInput;
        if (bytes == 0) return Status::Ok;
        if (!copyable(bytes, dst)) return Status::InvalidInput;
        if (!ensure_copy_buffers(bytes)) return Status::DeviceError;
        std::memcpy(in_.mapped, src, static_cast<std::size_t>(bytes));
        PushConstants pc{};
        pc.count = static_cast<std::uint32_t>(bytes / sizeof(float));
        const Borrowed to{static_cast<VkBuffer>(dst.handle), dst.offset, bytes};
        return dispatch(pipe_copy_, pc, pc.count, &to) ? Status::Ok : Status::DeviceError;
    }

    Status read_device_buffer(void* dst, const DeviceBuffer& src,
                              std::uint64_t bytes) override {
        if (owns_device_) return Status::Unsupported;
        if (src.empty() || !dst) return Status::InvalidInput;
        if (bytes == 0) return Status::Ok;
        if (!copyable(bytes, src)) return Status::InvalidInput;
        if (!ensure_copy_buffers(bytes)) return Status::DeviceError;
        PushConstants pc{};
        pc.count = static_cast<std::uint32_t>(bytes / sizeof(float));
        const Borrowed from{static_cast<VkBuffer>(src.handle), src.offset, bytes};
        if (!dispatch(pipe_copy_, pc, pc.count, nullptr, nullptr, &from))
            return Status::DeviceError;
        std::memcpy(dst, dist_.mapped, static_cast<std::size_t>(bytes));
        return Status::Ok;
    }

    Status raycast(const scene::Tape&, const RayQuery&, RayHit*) override {
        return Status::Unsupported;  // sphere tracing is templated C++ (field.h)
    }

  private:
    // -- setup ---------------------------------------------------------------

    bool init() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "claycore";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) return false;

        std::uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        if (n == 0) return false;
        std::vector<VkPhysicalDevice> devices(n);
        vkEnumeratePhysicalDevices(instance_, &n, devices.data());

        // Prefer a discrete GPU, then anything with a compute queue. A
        // software runtime (lavapipe) is last: it is the CPU, so picking it
        // over a real device would make "the GPU backend" a lie.
        int best_score = -1;
        for (VkPhysicalDevice dev : devices) {
            int family = compute_family(dev);
            if (family < 0) continue;
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(dev, &props);
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 3
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                     : 1;
            if (score > best_score) {
                best_score = score;
                physical_ = dev;
                queue_family_ = static_cast<std::uint32_t>(family);
                max_groups_ = props.limits.maxComputeWorkGroupCount[0];
            }
        }
        if (best_score < 0) return false;
        vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queue_family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) return false;
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        return build_pipelines() && build_pool();
    }

    // Adoption: take the caller's handles, check they are usable, and build
    // everything above them exactly as init() does. Validation is up front
    // because the alternative is a driver error deep inside a dispatch, in a
    // process the caller owns.
    bool adopt_init(const DeviceHandles& d) {
        instance_ = static_cast<VkInstance>(d.handles[0]);
        physical_ = static_cast<VkPhysicalDevice>(d.handles[1]);
        device_ = static_cast<VkDevice>(d.handles[2]);
        queue_ = static_cast<VkQueue>(d.handles[3]);
        queue_family_ = d.queue_family;
        owns_device_ = false;
        if (!instance_ || !physical_ || !device_ || !queue_) {
            device_ = VK_NULL_HANDLE;  // so the destructor touches nothing
            return false;
        }
        // The family the caller named must actually support compute: binding a
        // compute pipeline to a graphics-only queue is undefined behaviour
        // rather than an error the loader reports.
        std::uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &n, families.data());
        if (queue_family_ >= n || !(families[queue_family_].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            device_ = VK_NULL_HANDLE;
            return false;
        }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_, &props);
        max_groups_ = props.limits.maxComputeWorkGroupCount[0];
        vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);
        if (build_pipelines() && build_pool()) return true;
        return false;
    }

    int compute_family(VkPhysicalDevice dev) const {
        std::uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, families.data());
        for (std::uint32_t i = 0; i < n; ++i)
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) return static_cast<int>(i);
        return -1;
    }

    bool build_pipelines() {
        VkDescriptorSetLayoutBinding bindings[kBindings]{};
        for (std::uint32_t i = 0; i < kBindings; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = kBindings;
        dslci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout_) != VK_SUCCESS)
            return false;

        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        range.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set_layout_;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(device_, &plci, nullptr, &layout_) != VK_SUCCESS) return false;

        return make_pipeline(clay_spv_points, clay_spv_points_size, &pipe_points_) &&
               make_pipeline(clay_spv_grid, clay_spv_grid_size, &pipe_grid_) &&
               make_pipeline(clay_spv_copy, clay_spv_copy_size, &pipe_copy_);
    }

    bool make_pipeline(const unsigned char* spv, unsigned int bytes, VkPipeline* out) {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = bytes;
        smci.pCode = reinterpret_cast<const std::uint32_t*>(spv);
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &smci, nullptr, &module) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.layout = layout_;
        VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, out);
        vkDestroyShaderModule(device_, module, nullptr);
        return r == VK_SUCCESS;
    }

    bool build_pool() {
        VkDescriptorPoolSize size{};
        size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        size.descriptorCount = kBindings;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device_, &dpci, nullptr, &desc_pool_) != VK_SUCCESS)
            return false;
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(device_, &dsai, &set_) != VK_SUCCESS) return false;

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &cpci, nullptr, &pool_) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &cbai, &cmd_) != VK_SUCCESS) return false;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(device_, &fci, nullptr, &fence_) == VK_SUCCESS;
    }

    // -- buffers -------------------------------------------------------------

    int memory_type(std::uint32_t bits, VkMemoryPropertyFlags want) const {
        for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & want) == want)
                return static_cast<int>(i);
        return -1;
    }

    void destroy(Buffer& b) {
        if (b.mapped) vkUnmapMemory(device_, b.memory);
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
        b = Buffer{};
    }

    // Grow-in-place: a steady stream of same-sized dispatches allocates on
    // the first call and never again.
    bool ensure(Buffer* b, std::size_t bytes) {
        if (bytes == 0) bytes = 4;
        if (b->buffer && b->size >= bytes) return true;
        destroy(*b);
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &b->buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, b->buffer, &req);
        int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0) return false;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = static_cast<std::uint32_t>(type);
        if (vkAllocateMemory(device_, &mai, nullptr, &b->memory) != VK_SUCCESS) return false;
        if (vkBindBufferMemory(device_, b->buffer, b->memory, 0) != VK_SUCCESS) return false;
        if (vkMapMemory(device_, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS)
            return false;
        b->size = bytes;
        return true;
    }

    // A copy the shader can express: whole floats, a span the slice actually
    // holds, and an element count a 32-bit push constant can name.
    static bool copyable(std::uint64_t bytes, const DeviceBuffer& slice) {
        return bytes % sizeof(float) == 0 && slice.size >= bytes &&
               bytes / sizeof(float) <= 0xffffffffull;
    }

    // Every binding must name a real buffer for the descriptor write, including
    // the three the copy shader never touches. `ensure` only ever grows, so the
    // placeholders cost nothing once an evaluation has sized them.
    bool ensure_copy_buffers(std::uint64_t bytes) {
        const std::size_t span = static_cast<std::size_t>(bytes);
        return ensure(&instrs_, sizeof(float)) && ensure(&floats_, sizeof(float)) &&
               ensure(&color_, sizeof(float)) && ensure(&in_, span) && ensure(&dist_, span);
    }

    // The tape's params and blob live in ONE buffer, so the shader needs one
    // binding and the cursors are indices into it. The blob does NOT begin
    // where params end: it begins at a reserved capacity, with a gap between
    // them.
    //
    // The gap is what makes an append a tail write. A dab adds params, and
    // with the blob packed immediately behind them every append would shove
    // the whole blob right — which for a document of strokes or sampled
    // volumes is the 7.2 MiB this change exists to stop moving. The shader
    // reads blob_base as an opaque cursor (clay_kernels.comp.in), so the gap
    // costs nothing but the space it occupies and no shader change at all.
    // Room for what a stroke is about to add. Every tape buffer is sized
    // through this rather than to the tape's exact length, and that is not a
    // detail: an exact fit means the very next dab does not fit, the patch
    // declines, and the whole tape is re-uploaded — the allocation patching
    // exists to avoid, still paid on every stamp, just with less copied into
    // it. Measured before this: 3 patches and 5 whole uploads over 8 dabs.
    //
    // The two terms answer different documents. The PROPORTIONAL half keeps a
    // large tape from re-packing often — and because each re-pack reserves
    // half again, the re-packs over a stroke are geometric, so their cost is
    // amortised rather than paid per dab. The CONSTANT half is for a small
    // one, where half of very little is still not room for a single item: an
    // item is ~37 params, so 64 floats of slack absorbed two dabs and then
    // re-packed. 1024 absorbs a stroke's worth. It is 4 KiB a section, which
    // is nothing against the 7.8 MiB it stops re-sending.
    static std::size_t with_slack(std::size_t n) { return n + n / 2 + 1024; }

    // Where the tape goes, and how much of it actually has to be transferred.
    bool upload_tape(const scene::Tape& tape, PushConstants* pc) {
        pc->instr_count = static_cast<std::uint32_t>(tape.instrs.size());
        pc->params_base = 0;

        // Already here, byte for byte: this is the same compile.
        if (has_resident_ && tape.compile_id != 0 && tape.compile_id == resident_id_) {
            pc->blob_base = static_cast<std::uint32_t>(params_cap_);
            return true;
        }
        // Grew from what is here: transfer only what it does not share. The
        // patch declines when the buffers cannot hold the grown tape, and
        // that is a FALL-THROUGH, not a failure — growing a buffer hands back
        // a fresh allocation with nothing in it, so the prefix that was going
        // to be kept has to be re-sent.
        if (can_patch(tape) && patch_tape(tape)) {
            pc->blob_base = static_cast<std::uint32_t>(params_cap_);
            return true;
        }
        return upload_whole(tape, pc);
    }

    // A tape may be patched onto the resident one when it says so itself —
    // Tape::parent_id names the compile whose bytes it shares, and the
    // agree_* offsets say how many. Everything else about it is checked here
    // rather than trusted, because a patch written past what the two tapes
    // actually share evaluates a field that never existed, with no error and
    // no crash to say so.
    bool can_patch(const scene::Tape& tape) const {
        if (!has_resident_ || resident_id_ == 0) return false;
        if (tape.compile_id == 0 || tape.parent_id != resident_id_) return false;
        // Lineage says the tapes agree BELOW these; a tape cannot agree with
        // the resident one past its own length or past what was uploaded.
        if (tape.agree_instrs > tape.instrs.size() || tape.agree_params > tape.params.size() ||
            tape.agree_blob > tape.blob.size())
            return false;
        if (tape.agree_instrs > res_instrs_ || tape.agree_params > res_params_ ||
            tape.agree_blob > res_blob_)
            return false;
        // The params must still fit under the blob. When they do not, the
        // slack is spent and the tape is re-packed by an ordinary upload.
        return tape.params.size() <= params_cap_;
    }

    // The suffixes only, written into the persistently mapped buffers.
    bool patch_tape(const scene::Tape& tape) {
        const std::size_t instr_bytes = tape.instrs.size() * sizeof(kernel::CTapeInstr);
        // Declining, so the caller uploads whole: the buffers as they stand
        // have to hold the grown tape, because growing one would throw away
        // the prefix this patch is built on. ensure() grows geometrically, so
        // this is occasional rather than per-dab.
        if (instr_bytes > instrs_.size ||
            (params_cap_ + tape.blob.size()) * sizeof(float) > floats_.size)
            return false;

        if (tape.instrs.size() > tape.agree_instrs)
            std::memcpy(static_cast<kernel::CTapeInstr*>(instrs_.mapped) + tape.agree_instrs,
                        tape.instrs.data() + tape.agree_instrs,
                        (tape.instrs.size() - tape.agree_instrs) * sizeof(kernel::CTapeInstr));
        float* dst = static_cast<float*>(floats_.mapped);
        if (tape.params.size() > tape.agree_params)
            std::memcpy(dst + tape.agree_params, tape.params.data() + tape.agree_params,
                        (tape.params.size() - tape.agree_params) * sizeof(float));
        if (tape.blob.size() > tape.agree_blob)
            std::memcpy(dst + params_cap_ + tape.agree_blob, tape.blob.data() + tape.agree_blob,
                        (tape.blob.size() - tape.agree_blob) * sizeof(float));

        // The resident tape is now THIS one, which is what makes a stroke
        // cheap rather than only its first dab: the next append names this
        // compile as its parent, not the one uploaded whole several dabs ago.
        remember(tape);
        ++tape_patches_;
        return true;
    }

    bool upload_whole(const scene::Tape& tape, PushConstants* pc) {
        const std::size_t cap = with_slack(tape.params.size());
        if (!ensure(&instrs_, with_slack(tape.instrs.size()) * sizeof(kernel::CTapeInstr)))
            return false;
        if (!ensure(&floats_, (cap + with_slack(tape.blob.size())) * sizeof(float))) return false;
        if (!tape.instrs.empty())
            std::memcpy(instrs_.mapped, tape.instrs.data(),
                        tape.instrs.size() * sizeof(kernel::CTapeInstr));
        float* dst = static_cast<float*>(floats_.mapped);
        if (!tape.params.empty())
            std::memcpy(dst, tape.params.data(), tape.params.size() * sizeof(float));
        if (!tape.blob.empty())
            std::memcpy(dst + cap, tape.blob.data(), tape.blob.size() * sizeof(float));
        params_cap_ = cap;
        pc->blob_base = static_cast<std::uint32_t>(cap);
        remember(tape);
        ++tape_uploads_;
        return true;
    }

    // What is resident is now an IDENTITY, not a copy of the bytes.
    //
    // The compare this replaces was exact rather than a hash, and rightly so:
    // two different tapes that hashed alike would evaluate the wrong field
    // silently. But compile_id is not a hash — the compiler stamps it,
    // process-unique, so it cannot collide, which is what the Metal backend
    // has relied on since it was written. Keeping a full copy of the tape to
    // compare against cost 7.8 MiB of host memory at 50,000 items, for a
    // check that could never match once a stroke started changing the tape.
    void remember(const scene::Tape& tape) {
        resident_id_ = tape.compile_id;
        res_instrs_ = tape.instrs.size();
        res_params_ = tape.params.size();
        res_blob_ = tape.blob.size();
        has_resident_ = tape.compile_id != 0;
    }

    // -- dispatch ------------------------------------------------------------

    // A slice of a buffer we did not create, bound in place of one we did.
    // Non-owning by construction: it holds no memory and destroys nothing.
    struct Borrowed {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range = VK_WHOLE_SIZE;
    };

    // `dist` and `color` override bindings 3 and 4 when a caller asked for its
    // results in its own memory. Everything else binds exactly as before, so
    // the device path and the host path run the same shader over the same tape
    // and differ only in where the writes land — which is what makes them
    // comparable bit for bit.
    //
    // `in` overrides binding 2 the same way, and only the copy shader uses it:
    // reading a caller's buffer back is the same copy as writing it with the
    // two ends exchanged (#345).
    void bind_descriptors(const Borrowed* dist, const Borrowed* color, const Borrowed* in) {
        VkDescriptorBufferInfo infos[kBindings]{};
        const Buffer* buffers[kBindings] = {&instrs_, &floats_, &in_, &dist_, &color_};
        const Borrowed* overrides[kBindings] = {nullptr, nullptr, in, dist, color};
        VkWriteDescriptorSet writes[kBindings]{};
        for (std::uint32_t i = 0; i < kBindings; ++i) {
            if (overrides[i]) {
                infos[i].buffer = overrides[i]->buffer;
                infos[i].offset = overrides[i]->offset;
                infos[i].range = overrides[i]->range;
            } else {
                infos[i].buffer = buffers[i]->buffer;
                infos[i].range = VK_WHOLE_SIZE;
            }
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, kBindings, writes, 0, nullptr);
    }

    bool dispatch(VkPipeline pipeline, PushConstants pc, std::size_t elements,
                  const Borrowed* dist = nullptr, const Borrowed* color = nullptr,
                  const Borrowed* in = nullptr) {
        bind_descriptors(dist, color, in);
        // Split against the device's own limit rather than assuming a large
        // one: a 256^3 preview grid is 262144 groups, past what some devices
        // accept in a single dispatch.
        for (const vulkan_detail::DispatchChunk& chunk :
             vulkan_detail::dispatch_chunks(elements, kLocalSize, max_groups_)) {
            pc.base = chunk.first_element;

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkResetCommandBuffer(cmd_, 0) != VK_SUCCESS) return false;
            if (vkBeginCommandBuffer(cmd_, &begin) != VK_SUCCESS) return false;
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_,
                                    0, nullptr);
            vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(PushConstants), &pc);
            vkCmdDispatch(cmd_, chunk.groups, 1, 1);
            if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd_;
            if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return false;
            if (vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS) return false;
            if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    // False on an adopted device: the destructor must not destroy what the
    // caller lent us, and eval_grid_device is only meaningful when the buffers
    // and the device came from the same place.
    bool owns_device_ = true;
    std::uint64_t max_groups_ = 65535;
    VkPhysicalDeviceMemoryProperties mem_props_{};

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe_points_ = VK_NULL_HANDLE;
    VkPipeline pipe_grid_ = VK_NULL_HANDLE;
    VkPipeline pipe_copy_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    Buffer instrs_, floats_, in_, dist_, color_;

    bool has_resident_ = false;
    // Counts uploads, not dispatches. "The tape is uploaded when it changes"
    // is otherwise a claim no test can check, and an unchecked claim about a
    // cache is how a stale field ships. tape_patches_ is the same argument
    // for the half that transfers a suffix instead.
    std::uint64_t tape_uploads_ = 0;
    std::uint64_t tape_patches_ = 0;
    // The resident tape's identity and section lengths — what used to be a
    // full copy of its bytes.
    std::uint64_t resident_id_ = 0;
    std::size_t res_instrs_ = 0;
    std::size_t res_params_ = 0;
    std::size_t res_blob_ = 0;
    // Where the blob starts in floats_, which is the params capacity rather
    // than the params length: see params_capacity_for.
    std::size_t params_cap_ = 0;
};

}  // namespace

std::unique_ptr<Backend> create_vulkan_backend() { return VulkanBackend::create(); }

std::unique_ptr<Backend> adopt_vulkan_backend(const DeviceHandles& device) {
    return VulkanBackend::adopt(device);
}

// How many times the resident tape was actually re-uploaded. Exposed for the
// backend's own tests; a caller has no use for it and none is published in
// the C ABI.
std::uint64_t vulkan_tape_uploads(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).tape_uploads();
}

// How many times an appended tape was served by transferring only the suffix
// it did not share with the resident one. Same reason as the counter above:
// "a stroke uploads once and patches after that" is otherwise unfalsifiable.
std::uint64_t vulkan_tape_patches(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).tape_patches();
}

}  // namespace eval
}  // namespace clay
