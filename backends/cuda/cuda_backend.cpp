// CUDA backend host (evaluation-backends spec, tier 2: desktop/pipeline/ML
// batch workloads). Kernels are compiled by nvcc at build time from the same
// kernel headers as every other backend; this file owns buffers, launches,
// and registration. Registration fails softly when no CUDA device is
// present, so a CUDA-enabled build still runs everywhere.

#include <cuda_runtime.h>

#include <cstring>
#include <vector>

#include "clay/eval/backend.h"

#include "cuda_shared.h"

// Launch wrappers defined in clay_kernels.cu: the <<<>>> syntax only exists
// in nvcc's translation unit, so this host file calls plain functions.
extern "C" int clay_cuda_launch_eval_points(const void* instrs, const float* params,
                                            const float* blob, const float* pts, float* out_d,
                                            float* out_col, ClayCudaEvalUniforms u);
extern "C" int clay_cuda_launch_eval_grid(const void* instrs, const float* params,
                                          const float* blob, float* out_d, float* out_col,
                                          ClayCudaGridUniforms u);
extern "C" int clay_cuda_launch_raycast(const void* instrs, const float* params,
                                        const float* blob, const float* rays, void* hits,
                                        ClayCudaRayUniforms u);

namespace clay {
namespace eval {

namespace {

// RAII device buffer — no allocation survives a failed call.
class DeviceBuffer {
  public:
    DeviceBuffer() = default;
    ~DeviceBuffer() {
        if (ptr_) cudaFree(ptr_);
    }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    bool alloc(std::size_t bytes) {
        bytes_ = bytes ? bytes : 4;
        return cudaMalloc(&ptr_, bytes_) == cudaSuccess;
    }
    bool upload(const void* data, std::size_t bytes) {
        if (!alloc(bytes)) return false;
        if (bytes == 0) return true;
        return cudaMemcpy(ptr_, data, bytes, cudaMemcpyHostToDevice) == cudaSuccess;
    }
    bool download(void* dst, std::size_t bytes) const {
        if (bytes == 0) return true;
        return cudaMemcpy(dst, ptr_, bytes, cudaMemcpyDeviceToHost) == cudaSuccess;
    }
    template <typename T>
    T* as() const {
        return static_cast<T*>(ptr_);
    }

  private:
    void* ptr_ = nullptr;
    std::size_t bytes_ = 0;
};

struct TapeBuffers {
    DeviceBuffer instrs, params, blob;

    bool upload(const scene::Tape& tape) {
        return instrs.upload(tape.instrs.data(),
                             tape.instrs.size() * sizeof(kernel::CTapeInstr)) &&
               params.upload(tape.params.data(), tape.params.size() * sizeof(float)) &&
               blob.upload(tape.blob.data(), tape.blob.size() * sizeof(float));
    }
};

class CudaBackend final : public Backend {
  public:
    static std::unique_ptr<CudaBackend> create() {
        int count = 0;
        if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) return nullptr;
        if (cudaSetDevice(0) != cudaSuccess) return nullptr;
        return std::unique_ptr<CudaBackend>(new CudaBackend());
    }

    const char* name() const override { return "cuda"; }
    BackendCaps caps() const override { return BackendCaps{false, true, 0}; }

    Status eval_points(const scene::Tape& tape, const PointQuery& q,
                       const PointResults& out) override {
        if (!q.points_xyz || !out.distances) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;

        TapeBuffers tb;
        DeviceBuffer pts, dist, cols;
        if (!tb.upload(tape) || !pts.upload(q.points_xyz, q.count * 3 * sizeof(float)) ||
            !dist.alloc(q.count * sizeof(float)) ||
            !cols.alloc((out.colors_rgb ? q.count * 3 : 1) * sizeof(float)))
            return Status::DeviceError;

        ClayCudaEvalUniforms u{};
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.point_count = static_cast<unsigned int>(q.count);
        u.has_colors = out.colors_rgb ? 1u : 0u;

        if (clay_cuda_launch_eval_points(tb.instrs.as<const void>(), tb.params.as<const float>(),
                                         tb.blob.as<const float>(), pts.as<const float>(),
                                         dist.as<float>(), cols.as<float>(), u) != 0)
            return Status::DeviceError;

        if (!dist.download(out.distances, q.count * sizeof(float))) return Status::DeviceError;
        if (out.colors_rgb && !cols.download(out.colors_rgb, q.count * 3 * sizeof(float)))
            return Status::DeviceError;
        if (out.gradients_xyz) {  // tetrahedron taps on the host tape
            PointResults grad = out;
            std::vector<float> tmp(q.count);
            grad.distances = tmp.data();
            grad.colors_rgb = nullptr;
            eval_points_reference(tape, q, grad);
        }
        return Status::Ok;
    }

    Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                     float* out_colors_rgb) override {
        if (!out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        std::size_t total = static_cast<std::size_t>(q.nx) * q.ny * q.nz;

        TapeBuffers tb;
        DeviceBuffer dist, cols;
        if (!tb.upload(tape) || !dist.alloc(total * sizeof(float)) ||
            !cols.alloc((out_colors_rgb ? total * 3 : 1) * sizeof(float)))
            return Status::DeviceError;

        ClayCudaGridUniforms u{};
        u.origin[0] = q.origin.x;
        u.origin[1] = q.origin.y;
        u.origin[2] = q.origin.z;
        u.spacing = q.spacing;
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.nx = static_cast<unsigned int>(q.nx);
        u.ny = static_cast<unsigned int>(q.ny);
        u.nz = static_cast<unsigned int>(q.nz);
        u.has_colors = out_colors_rgb ? 1u : 0u;

        if (clay_cuda_launch_eval_grid(tb.instrs.as<const void>(), tb.params.as<const float>(),
                                       tb.blob.as<const float>(), dist.as<float>(),
                                       cols.as<float>(), u) != 0)
            return Status::DeviceError;

        if (!dist.download(out_values, total * sizeof(float))) return Status::DeviceError;
        if (out_colors_rgb && !cols.download(out_colors_rgb, total * 3 * sizeof(float)))
            return Status::DeviceError;
        return Status::Ok;
    }

    Status raycast(const scene::Tape& tape, const RayQuery& q, RayHit* hits) override {
        if (!q.rays || !hits) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;
        static_assert(sizeof(RayHit) == sizeof(ClayCudaRayHit));

        TapeBuffers tb;
        DeviceBuffer rays, out;
        if (!tb.upload(tape) || !rays.upload(q.rays, q.count * 6 * sizeof(float)) ||
            !out.alloc(q.count * sizeof(ClayCudaRayHit)))
            return Status::DeviceError;

        ClayCudaRayUniforms u{};
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

        if (clay_cuda_launch_raycast(tb.instrs.as<const void>(), tb.params.as<const float>(),
                                     tb.blob.as<const float>(), rays.as<const float>(),
                                     out.as<void>(), u) != 0)
            return Status::DeviceError;
        return out.download(hits, q.count * sizeof(ClayCudaRayHit)) ? Status::Ok
                                                                    : Status::DeviceError;
    }
};

}  // namespace

std::unique_ptr<Backend> create_cuda_backend() { return CudaBackend::create(); }

}  // namespace eval
}  // namespace clay
