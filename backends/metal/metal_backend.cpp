// Metal backend host — pure C++ via metal-cpp, no Objective-C. Loads the
// embedded metallib (compiled at build time from the shared kernel headers)
// and dispatches the fixed tape-interpreter kernels.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <cstdio>
#include <string>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>

#include <cstring>
#include <vector>

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

    // Bound to a device the CALLER owns. The pipelines and the library are
    // still ours and are still built and released here; only the device and
    // the command queue are borrowed.
    static std::unique_ptr<MetalBackend> adopt(const DeviceHandles& d) {
        auto b = std::unique_ptr<MetalBackend>(new MetalBackend());
        return b->adopt_init(d) ? std::move(b) : nullptr;
    }

    ~MetalBackend() override {
        if (pso_points_) pso_points_->release();
        if (pso_grid_) pso_grid_->release();
        if (pso_rays_) pso_rays_->release();
        // Release nothing we did not retain. On an adopted device the caller
        // keeps its device and queue, and releasing them here would drop a
        // reference the host still holds.
        if (owns_device_) {
            if (queue_) queue_->release();
            if (device_) device_->release();
        }
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

    // Results land in the caller's own MTLBuffer, so a host that was going to
    // draw them never copies them through host memory. Same shader, same tape,
    // same uniforms as eval_grid — only the destination differs.
    Status eval_grid_device(const scene::Tape& tape, const GridQuery& q,
                            const DeviceBuffer& values, const DeviceBuffer& colors) override {
        // Only an ADOPTED backend can serve this: an MTLBuffer belongs to the
        // device that made it.
        if (owns_device_) return Status::Unsupported;
        if (values.empty() || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        const std::size_t total = static_cast<std::size_t>(q.nx) *
                                  static_cast<std::size_t>(q.ny) *
                                  static_cast<std::size_t>(q.nz);
        // Checked before any dispatch, so an undersized destination is refused
        // with nothing written rather than partially filled.
        if (values.size < static_cast<std::uint64_t>(total) * sizeof(float))
            return Status::InvalidInput;
        const bool want_colors = !colors.empty();
        if (want_colors && colors.size < static_cast<std::uint64_t>(total) * 3 * sizeof(float))
            return Status::InvalidInput;

        ClayGridUniforms u{};
        u.origin[0] = q.origin.x;
        u.origin[1] = q.origin.y;
        u.origin[2] = q.origin.z;
        u.spacing = q.spacing;
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.nx = static_cast<unsigned int>(q.nx);
        u.ny = static_cast<unsigned int>(q.ny);
        u.nz = static_cast<unsigned int>(q.nz);
        u.has_colors = want_colors ? 1u : 0u;

        TapeBuffers tb = upload_tape(tape);
        // Binding 4 must be a real buffer even when no colours were asked for.
        MTL::Buffer* scratch =
            want_colors ? nullptr : device_->newBuffer(4, MTL::ResourceStorageModeShared);
        const std::vector<Bound> bound = {
            Bound{tb.instrs, 0}, Bound{tb.params, 0}, Bound{tb.blob, 0},
            Bound{static_cast<MTL::Buffer*>(values.handle),
                  static_cast<NS::UInteger>(values.offset)},
            want_colors ? Bound{static_cast<MTL::Buffer*>(colors.handle),
                                static_cast<NS::UInteger>(colors.offset)}
                        : Bound{scratch, 0},
        };
        const bool ok = dispatch_bound(pso_grid_, bound, &u, sizeof(u), 5, total);
        // dispatch waits until completed, so the work has landed here and
        // nothing is left in flight on the caller's queue.
        release_all({tb.instrs, tb.params, tb.blob, scratch});
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
        if (!build_pipelines()) return false;
        queue_ = device_->newCommandQueue();
        return queue_ != nullptr;
    }

    bool adopt_init(const DeviceHandles& d) {
        device_ = static_cast<MTL::Device*>(d.handles[0]);
        queue_ = static_cast<MTL::CommandQueue*>(d.handles[1]);
        owns_device_ = false;
        if (!device_ || !queue_) {
            device_ = nullptr;  // so the destructor touches nothing
            queue_ = nullptr;
            return false;
        }
        return build_pipelines();
    }

    // Every failure below used to discard the NS::Error it was handed, so a
    // backend that could not initialise registered nothing and said nothing —
    // indistinguishable from a machine with no GPU. That cost three release
    // attempts to diagnose: the artifact was fine and the reason was sitting
    // in an ignored out-parameter. Metal init happens once, so one line on
    // stderr is cheap and is the difference between a mystery and a message.
    static void report(const char* what, NS::Error* err) {
        const char* detail = "no error reported";
        if (err && err->localizedDescription())
            detail = err->localizedDescription()->utf8String();
        std::fprintf(stderr, "claycore: the Metal backend will not register — %s: %s\n", what,
                     detail);
    }

    bool build_pipelines() {
        dispatch_data_t data = dispatch_data_create(clay_metallib, clay_metallib_size, nullptr,
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NS::Error* err = nullptr;
        MTL::Library* lib = device_->newLibrary(data, &err);
        dispatch_release(data);
        if (!lib) {
            report("the embedded metallib did not load", err);
            return false;
        }
        pso_points_ = make_pso(lib, "clay_eval_points");
        pso_grid_ = make_pso(lib, "clay_eval_grid");
        pso_rays_ = make_pso(lib, "clay_raycast");
        lib->release();
        return pso_points_ && pso_grid_ && pso_rays_;
    }

    MTL::ComputePipelineState* make_pso(MTL::Library* lib, const char* fn_name) {
        NS::String* name = NS::String::string(fn_name, NS::UTF8StringEncoding);
        MTL::Function* fn = lib->newFunction(name);
        if (!fn) {
            std::fprintf(stderr,
                         "claycore: the Metal backend will not register — the metallib "
                         "carries no function named %s\n",
                         fn_name);
            return nullptr;
        }
        NS::Error* err = nullptr;
        MTL::ComputePipelineState* pso = device_->newComputePipelineState(fn, &err);
        fn->release();
        if (!pso) {
            std::string what = std::string("no compute pipeline for ") + fn_name;
            report(what.c_str(), err);
        }
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

    // A buffer and where in it to start. Offsets matter only on the device
    // path, where a host packs many results into one allocation.
    struct Bound {
        MTL::Buffer* buffer = nullptr;
        NS::UInteger offset = 0;
    };

    bool dispatch(MTL::ComputePipelineState* pso, std::initializer_list<MTL::Buffer*> buffers,
                  const void* uniforms, std::size_t uniform_bytes, unsigned uniform_index,
                  std::size_t thread_count) {
        std::vector<Bound> bound;
        bound.reserve(buffers.size());
        for (MTL::Buffer* b : buffers) bound.push_back(Bound{b, 0});
        return dispatch_bound(pso, bound, uniforms, uniform_bytes, uniform_index, thread_count);
    }

    bool dispatch_bound(MTL::ComputePipelineState* pso, const std::vector<Bound>& buffers,
                        const void* uniforms, std::size_t uniform_bytes,
                        unsigned uniform_index, std::size_t thread_count) {
        MTL::CommandBuffer* cmd = queue_->commandBuffer();
        if (!cmd) return false;
        MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();
        enc->setComputePipelineState(pso);
        unsigned idx = 0;
        for (const Bound& b : buffers) enc->setBuffer(b.buffer, b.offset, idx++);
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
    // False on an adopted device: the destructor must not release what the
    // caller lent us, and eval_grid_device is only meaningful when the buffers
    // and the device came from the same place.
    bool owns_device_ = true;
    MTL::ComputePipelineState* pso_points_ = nullptr;
    MTL::ComputePipelineState* pso_grid_ = nullptr;
    MTL::ComputePipelineState* pso_rays_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> create_metal_backend() { return MetalBackend::create(); }

std::unique_ptr<Backend> adopt_metal_backend(const DeviceHandles& device) {
    return MetalBackend::adopt(device);
}

}  // namespace eval
}  // namespace clay
