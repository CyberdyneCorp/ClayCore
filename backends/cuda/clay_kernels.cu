// CUDA tape-interpreter kernels — compiled by nvcc from the SAME kernel
// headers as every other backend (single-source rule). Built without
// fast-math so parity with the CPU scalar reference holds within 1e-4.

#define CLAY_KERNEL_CUDA 1
#include "clay/kernel/field.h"
#include "clay/kernel/tape.h"

#include "cuda_shared.h"

using namespace clay::kernel;

namespace {

// Field functor over device tape buffers (works with the templated
// craycast/cnormal utilities — CUDA compiles C++ templates, unlike OpenCL).
struct TapeField {
    const CTapeInstr* instrs;
    int count;
    const float* params;
    const float* strokes;
    __device__ float operator()(cfloat3 p) const {
        return ctape_eval(instrs, count, params, strokes, p).d;
    }
};

}  // namespace

static __global__ void clay_eval_points_kernel(const CTapeInstr* instrs,
                                                   const float* params, const float* strokes,
                                                   const float* pts, float* out_d,
                                                   float* out_col, ClayCudaEvalUniforms u) {
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= u.point_count) return;
    cfloat3 p = cf3(pts[gid * 3 + 0], pts[gid * 3 + 1], pts[gid * 3 + 2]);
    CTapeValue v = ctape_eval(instrs, (int)u.instr_count, params, strokes, p);
    out_d[gid] = v.d;
    if (u.has_colors) {
        out_col[gid * 3 + 0] = v.color.x;
        out_col[gid * 3 + 1] = v.color.y;
        out_col[gid * 3 + 2] = v.color.z;
    }
}

static __global__ void clay_eval_grid_kernel(const CTapeInstr* instrs, const float* params,
                                                 const float* strokes, float* out_d,
                                                 float* out_col, ClayCudaGridUniforms u) {
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned int total = u.nx * u.ny * u.nz;
    if (gid >= total) return;
    unsigned int x = gid % u.nx;
    unsigned int y = (gid / u.nx) % u.ny;
    unsigned int z = gid / (u.nx * u.ny);
    cfloat3 p = cf3(u.origin[0], u.origin[1], u.origin[2]) +
                cf3((float)x, (float)y, (float)z) * u.spacing;
    CTapeValue v = ctape_eval(instrs, (int)u.instr_count, params, strokes, p);
    out_d[gid] = v.d;
    if (u.has_colors) {
        out_col[gid * 3 + 0] = v.color.x;
        out_col[gid * 3 + 1] = v.color.y;
        out_col[gid * 3 + 2] = v.color.z;
    }
}

static __global__ void clay_raycast_kernel(const CTapeInstr* instrs, const float* params,
                                               const float* strokes, const float* rays,
                                               ClayCudaRayHit* hits, ClayCudaRayUniforms u) {
    unsigned int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= u.ray_count) return;
    cfloat3 ro = cf3(rays[gid * 6 + 0], rays[gid * 6 + 1], rays[gid * 6 + 2]);
    cfloat3 rd = cf3(rays[gid * 6 + 3], rays[gid * 6 + 4], rays[gid * 6 + 5]);

    ClayCudaRayHit hit;
    hit.hit = 0;
    hit.t = 0.0f;
    hit.pos[0] = hit.pos[1] = hit.pos[2] = 0.0f;
    hit.normal[0] = hit.normal[1] = hit.normal[2] = 0.0f;

    float tmin = u.tmin, tmax = u.tmax;
    if (u.has_bounds) {  // slab clip, mirroring the CPU backend
        float lo = tmin, hi = tmax;
        bool miss = false;
        for (int i = 0; i < 3; ++i) {
            float roi = i == 0 ? ro.x : (i == 1 ? ro.y : ro.z);
            float rdi = i == 0 ? rd.x : (i == 1 ? rd.y : rd.z);
            float mni = u.bounds_min[i];
            float mxi = u.bounds_max[i];
            if (cabs(rdi) < 1e-12f) {
                if (roi < mni || roi > mxi) miss = true;
                continue;
            }
            float inv = 1.0f / rdi;
            float a = (mni - roi) * inv;
            float b = (mxi - roi) * inv;
            if (a > b) {
                float tmp = a;
                a = b;
                b = tmp;
            }
            lo = cmax(lo, a);
            hi = cmin(hi, b);
        }
        if (miss || lo > hi) {
            hits[gid] = hit;
            return;
        }
        tmin = lo;
        tmax = hi;
    }

    TapeField field{instrs, (int)u.instr_count, params, strokes};
    CRayHit r = craycast(field, ro, rd, tmin, tmax, u.eps, u.step_scale, 1.4f, (int)u.max_steps);
    if (r.hit) {
        hit.hit = 1;
        hit.t = r.t;
        cfloat3 pos = ro + rd * r.t;
        cfloat3 n = cnormal(field, pos, 1e-4f);
        hit.pos[0] = pos.x;
        hit.pos[1] = pos.y;
        hit.pos[2] = pos.z;
        hit.normal[0] = n.x;
        hit.normal[1] = n.y;
        hit.normal[2] = n.z;
    }
    hits[gid] = hit;
}

// -- launchers ---------------------------------------------------------------
// Kernel launch syntax only exists inside nvcc's translation unit, so the
// host backend calls these plain wrappers instead. Each synchronizes and
// returns a cudaError_t as int.

namespace {
constexpr int kBlock = 128;
int blocks(unsigned int items) { return (int)((items + kBlock - 1) / kBlock); }
}  // namespace

extern "C" int clay_cuda_launch_eval_points(const void* instrs, const float* params,
                                            const float* strokes, const float* pts, float* out_d,
                                            float* out_col, ClayCudaEvalUniforms u) {
    clay_eval_points_kernel<<<blocks(u.point_count), kBlock>>>(
        (const CTapeInstr*)instrs, params, strokes, pts, out_d, out_col, u);
    return (int)cudaDeviceSynchronize();
}

extern "C" int clay_cuda_launch_eval_grid(const void* instrs, const float* params,
                                          const float* strokes, float* out_d, float* out_col,
                                          ClayCudaGridUniforms u) {
    unsigned int total = u.nx * u.ny * u.nz;
    clay_eval_grid_kernel<<<blocks(total), kBlock>>>((const CTapeInstr*)instrs, params, strokes,
                                                     out_d, out_col, u);
    return (int)cudaDeviceSynchronize();
}

extern "C" int clay_cuda_launch_raycast(const void* instrs, const float* params,
                                        const float* strokes, const float* rays, void* hits,
                                        ClayCudaRayUniforms u) {
    clay_raycast_kernel<<<blocks(u.ray_count), kBlock>>>((const CTapeInstr*)instrs, params,
                                                         strokes, rays,
                                                         (ClayCudaRayHit*)hits, u);
    return (int)cudaDeviceSynchronize();
}
