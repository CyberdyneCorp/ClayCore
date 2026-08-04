// Metal backend host — pure C++ via metal-cpp, no Objective-C. Loads the
// embedded metallib (compiled at build time from the shared kernel headers)
// and dispatches the fixed tape-interpreter kernels.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>

#include <cstring>

#include "clay/eval/backend.h"
#include "metal_shared.h"

#include "../common/grid_mesh.h"

extern "C" {
extern const unsigned char clay_metallib[];
extern const unsigned int clay_metallib_size;
}

namespace clay {
namespace eval {

namespace {

class MetalBackend final : public Backend {
  public:
    static std::unique_ptr<MetalBackend> create() {
        auto b = std::unique_ptr<MetalBackend>(new MetalBackend());
        return b->init() ? std::move(b) : nullptr;
    }

    ~MetalBackend() override {
        if (pso_points_) pso_points_->release();
        if (pso_grid_) pso_grid_->release();
        if (pso_rays_) pso_rays_->release();
        if (queue_) queue_->release();
        if (device_) device_->release();
    }

    const char* name() const override { return "metal"; }
    BackendCaps caps() const override { return BackendCaps{false, true, 0}; }

    Status eval_points(const scene::Tape& tape, const PointQuery& q,
                       const PointResults& out) override {
        if (!q.points_xyz || !out.distances) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;

        ClayEvalUniforms u{};
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.point_count = static_cast<unsigned int>(q.count);
        u.has_colors = out.colors_rgb ? 1u : 0u;

        MTL::Buffer* pts = copy_in(q.points_xyz, q.count * 3 * sizeof(float));
        MTL::Buffer* dist = device_->newBuffer(q.count * sizeof(float),
                                               MTL::ResourceStorageModeShared);
        MTL::Buffer* cols = device_->newBuffer(
            (out.colors_rgb ? q.count * 3 : 1) * sizeof(float), MTL::ResourceStorageModeShared);

        TapeBuffers tb = upload_tape(tape);
        bool ok = dispatch(pso_points_, {tb.instrs, tb.params, tb.blob, pts, dist, cols},
                           &u, sizeof(u), 6, q.count);
        if (ok) {
            std::memcpy(out.distances, dist->contents(), q.count * sizeof(float));
            if (out.colors_rgb)
                std::memcpy(out.colors_rgb, cols->contents(), q.count * 3 * sizeof(float));
            if (out.gradients_xyz) gradients_from_taps(tape, q, out);
        }
        release_all({tb.instrs, tb.params, tb.blob, pts, dist, cols});
        return ok ? Status::Ok : Status::DeviceError;
    }

    Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                     float* out_colors_rgb) override {
        if (!out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        std::size_t total = static_cast<std::size_t>(q.nx) * q.ny * q.nz;

        ClayGridUniforms u{};
        u.origin[0] = q.origin.x;
        u.origin[1] = q.origin.y;
        u.origin[2] = q.origin.z;
        u.spacing = q.spacing;
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.nx = static_cast<unsigned int>(q.nx);
        u.ny = static_cast<unsigned int>(q.ny);
        u.nz = static_cast<unsigned int>(q.nz);
        u.has_colors = out_colors_rgb ? 1u : 0u;

        MTL::Buffer* dist = device_->newBuffer(total * sizeof(float),
                                               MTL::ResourceStorageModeShared);
        MTL::Buffer* cols = device_->newBuffer(
            (out_colors_rgb ? total * 3 : 1) * sizeof(float), MTL::ResourceStorageModeShared);
        TapeBuffers tb = upload_tape(tape);
        bool ok = dispatch(pso_grid_, {tb.instrs, tb.params, tb.blob, dist, cols}, &u,
                           sizeof(u), 5, total);
        if (ok) {
            std::memcpy(out_values, dist->contents(), total * sizeof(float));
            if (out_colors_rgb)
                std::memcpy(out_colors_rgb, cols->contents(), total * 3 * sizeof(float));
        }
        release_all({tb.instrs, tb.params, tb.blob, dist, cols});
        return ok ? Status::Ok : Status::DeviceError;
    }

    Status mesh(const scene::Tape& tape, const GridQuery& q, std::vector<float>* out_verts,
                std::vector<std::uint32_t>* out_indices) override {
        // hybrid: field values on the device, triangulation on the host
        return grid_mesh(*this, tape, q, out_verts, out_indices);
    }

    Status raycast(const scene::Tape& tape, const RayQuery& q, RayHit* hits) override {
        if (!q.rays || !hits) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;
        static_assert(sizeof(RayHit) == sizeof(ClayRayHitGpu));

        ClayRayUniforms u{};
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.ray_count = static_cast<unsigned int>(q.count);
        u.max_steps = static_cast<unsigned int>(q.max_steps);
        u.tmin = q.tmin;
        u.tmax = q.tmax;
        u.eps = q.eps;
        u.step_scale = tape.safe_step_scale();
        if (!tape.bounds.empty() && !tape.bounds.is_infinite()) {
            math::Aabb b = tape.bounds.dilated(0.01f);
            u.has_bounds = 1;
            u.bounds_min[0] = b.min.x;
            u.bounds_min[1] = b.min.y;
            u.bounds_min[2] = b.min.z;
            u.bounds_max[0] = b.max.x;
            u.bounds_max[1] = b.max.y;
            u.bounds_max[2] = b.max.z;
        }

        MTL::Buffer* rays = copy_in(q.rays, q.count * 6 * sizeof(float));
        MTL::Buffer* out = device_->newBuffer(q.count * sizeof(ClayRayHitGpu),
                                              MTL::ResourceStorageModeShared);
        TapeBuffers tb = upload_tape(tape);
        bool ok = dispatch(pso_rays_, {tb.instrs, tb.params, tb.blob, rays, out}, &u,
                           sizeof(u), 5, q.count);
        if (ok) std::memcpy(hits, out->contents(), q.count * sizeof(ClayRayHitGpu));
        release_all({tb.instrs, tb.params, tb.blob, rays, out});
        return ok ? Status::Ok : Status::DeviceError;
    }

  private:
    MetalBackend() = default;

    struct TapeBuffers {
        MTL::Buffer* instrs;
        MTL::Buffer* params;
        MTL::Buffer* blob;
    };

    bool init() {
        device_ = MTL::CreateSystemDefaultDevice();
        if (!device_) return false;
        dispatch_data_t data = dispatch_data_create(clay_metallib, clay_metallib_size, nullptr,
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NS::Error* err = nullptr;
        MTL::Library* lib = device_->newLibrary(data, &err);
        dispatch_release(data);
        if (!lib) return false;
        pso_points_ = make_pso(lib, "clay_eval_points");
        pso_grid_ = make_pso(lib, "clay_eval_grid");
        pso_rays_ = make_pso(lib, "clay_raycast");
        lib->release();
        queue_ = device_->newCommandQueue();
        return pso_points_ && pso_grid_ && pso_rays_ && queue_;
    }

    MTL::ComputePipelineState* make_pso(MTL::Library* lib, const char* fn_name) {
        NS::String* name = NS::String::string(fn_name, NS::UTF8StringEncoding);
        MTL::Function* fn = lib->newFunction(name);
        if (!fn) return nullptr;
        NS::Error* err = nullptr;
        MTL::ComputePipelineState* pso = device_->newComputePipelineState(fn, &err);
        fn->release();
        return pso;
    }

    MTL::Buffer* copy_in(const void* data, std::size_t bytes) {
        if (bytes == 0) return device_->newBuffer(4, MTL::ResourceStorageModeShared);
        return device_->newBuffer(data, bytes, MTL::ResourceStorageModeShared);
    }

    TapeBuffers upload_tape(const scene::Tape& tape) {
        return TapeBuffers{
            copy_in(tape.instrs.data(), tape.instrs.size() * sizeof(kernel::CTapeInstr)),
            copy_in(tape.params.data(), tape.params.size() * sizeof(float)),
            copy_in(tape.blob.data(), tape.blob.size() * sizeof(float)),
        };
    }

    bool dispatch(MTL::ComputePipelineState* pso, std::initializer_list<MTL::Buffer*> buffers,
                  const void* uniforms, std::size_t uniform_bytes, unsigned uniform_index,
                  std::size_t thread_count) {
        MTL::CommandBuffer* cmd = queue_->commandBuffer();
        if (!cmd) return false;
        MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();
        enc->setComputePipelineState(pso);
        unsigned idx = 0;
        for (MTL::Buffer* b : buffers) enc->setBuffer(b, 0, idx++);
        enc->setBytes(uniforms, uniform_bytes, uniform_index);
        MTL::Size grid(thread_count, 1, 1);
        NS::UInteger tg = pso->maxTotalThreadsPerThreadgroup();
        if (tg > 64) tg = 64;
        enc->dispatchThreads(grid, MTL::Size(tg, 1, 1));
        enc->endEncoding();
        cmd->commit();
        cmd->waitUntilCompleted();
        return cmd->status() == MTL::CommandBufferStatusCompleted;
    }

    void release_all(std::initializer_list<MTL::Buffer*> buffers) {
        for (MTL::Buffer* b : buffers)
            if (b) b->release();
    }

    // Gradients on the host from 4 extra device evaluations would need a
    // second dispatch; for now derive them CPU-side from the tape (rarely
    // requested on the GPU path — brick fills don't need them).
    void gradients_from_taps(const scene::Tape& tape, const PointQuery& q,
                             const PointResults& out) {
        PointResults grad_only;
        grad_only.gradients_xyz = out.gradients_xyz;
        PointQuery sub = q;
        std::vector<float> tmp(q.count);
        grad_only.distances = tmp.data();
        eval_points_reference(tape, sub, grad_only);
    }

    MTL::Device* device_ = nullptr;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::ComputePipelineState* pso_points_ = nullptr;
    MTL::ComputePipelineState* pso_grid_ = nullptr;
    MTL::ComputePipelineState* pso_rays_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> create_metal_backend() { return MetalBackend::create(); }

}  // namespace eval
}  // namespace clay
