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

    ~VulkanBackend() override {
        if (device_) {
            vkDeviceWaitIdle(device_);
            for (Buffer* b : {&instrs_, &floats_, &in_, &dist_, &color_}) destroy(*b);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
            if (pipe_points_) vkDestroyPipeline(device_, pipe_points_, nullptr);
            if (pipe_grid_) vkDestroyPipeline(device_, pipe_grid_, nullptr);
            if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
            if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
    }

    const char* name() const override { return "vulkan"; }
    std::uint64_t tape_uploads() const { return tape_uploads_; }
    BackendCaps caps() const override { return BackendCaps{false, false, 0}; }

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
               make_pipeline(clay_spv_grid, clay_spv_grid_size, &pipe_grid_);
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

    // The tape's params and blob live in ONE buffer, in that order, so the
    // shader needs one binding and the cursors are indices into it.
    bool upload_tape(const scene::Tape& tape, PushConstants* pc) {
        pc->instr_count = static_cast<std::uint32_t>(tape.instrs.size());
        pc->params_base = 0;
        pc->blob_base = static_cast<std::uint32_t>(tape.params.size());

        const std::size_t floats = tape.params.size() + tape.blob.size();
        if (resident(tape)) return true;

        if (!ensure(&instrs_, tape.instrs.size() * sizeof(kernel::CTapeInstr))) return false;
        if (!ensure(&floats_, floats * sizeof(float))) return false;
        if (!tape.instrs.empty())
            std::memcpy(instrs_.mapped, tape.instrs.data(),
                        tape.instrs.size() * sizeof(kernel::CTapeInstr));
        float* dst = static_cast<float*>(floats_.mapped);
        if (!tape.params.empty())
            std::memcpy(dst, tape.params.data(), tape.params.size() * sizeof(float));
        if (!tape.blob.empty())
            std::memcpy(dst + tape.params.size(), tape.blob.data(),
                        tape.blob.size() * sizeof(float));
        remember(tape);
        ++tape_uploads_;
        return true;
    }

    // An exact compare, not a hash. Two different tapes that hash alike would
    // evaluate the wrong field silently, which is worse than any upload it
    // could save; the compare touches the same bytes the upload would.
    bool resident(const scene::Tape& tape) const {
        return has_resident_ && res_instrs_.size() == tape.instrs.size() &&
               res_params_.size() == tape.params.size() &&
               res_blob_.size() == tape.blob.size() &&
               std::memcmp(res_instrs_.data(), tape.instrs.data(),
                           res_instrs_.size() * sizeof(kernel::CTapeInstr)) == 0 &&
               std::memcmp(res_params_.data(), tape.params.data(),
                           res_params_.size() * sizeof(float)) == 0 &&
               std::memcmp(res_blob_.data(), tape.blob.data(),
                           res_blob_.size() * sizeof(float)) == 0;
    }

    void remember(const scene::Tape& tape) {
        res_instrs_ = tape.instrs;
        res_params_ = tape.params;
        res_blob_ = tape.blob;
        has_resident_ = true;
    }

    // -- dispatch ------------------------------------------------------------

    void bind_descriptors() {
        VkDescriptorBufferInfo infos[kBindings]{};
        const Buffer* buffers[kBindings] = {&instrs_, &floats_, &in_, &dist_, &color_};
        VkWriteDescriptorSet writes[kBindings]{};
        for (std::uint32_t i = 0; i < kBindings; ++i) {
            infos[i].buffer = buffers[i]->buffer;
            infos[i].range = VK_WHOLE_SIZE;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, kBindings, writes, 0, nullptr);
    }

    bool dispatch(VkPipeline pipeline, PushConstants pc, std::size_t elements) {
        bind_descriptors();
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
    std::uint64_t max_groups_ = 65535;
    VkPhysicalDeviceMemoryProperties mem_props_{};

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe_points_ = VK_NULL_HANDLE;
    VkPipeline pipe_grid_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    Buffer instrs_, floats_, in_, dist_, color_;

    bool has_resident_ = false;
    // Counts uploads, not dispatches. "The tape is uploaded when it changes"
    // is otherwise a claim no test can check, and an unchecked claim about a
    // cache is how a stale field ships.
    std::uint64_t tape_uploads_ = 0;
    std::vector<kernel::CTapeInstr> res_instrs_;
    std::vector<float> res_params_;
    std::vector<float> res_blob_;
};

}  // namespace

std::unique_ptr<Backend> create_vulkan_backend() { return VulkanBackend::create(); }

// How many times the resident tape was actually re-uploaded. Exposed for the
// backend's own tests; a caller has no use for it and none is published in
// the C ABI.
std::uint64_t vulkan_tape_uploads(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).tape_uploads();
}

}  // namespace eval
}  // namespace clay
