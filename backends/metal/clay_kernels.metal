// Metal tape-interpreter kernels — compiled from the SAME kernel headers as
// every other backend (single-source rule). Built with -fno-fast-math so
// parity with the CPU scalar reference stays within 1e-4.

#define CLAY_KERNEL_METAL 1
#include "clay/kernel/field.h"
#include "clay/kernel/tape.h"
#include "metal_shared.h"

using namespace clay::kernels;  // MSL reserves `kernel` — see shim.h

namespace {

// Field functor over device tape buffers (works with the templated
// craycast/cnormal utilities).
struct TapeField {
    device const CTapeInstr* instrs;
    int count;
    device const float* params;
    device const float* blob;
    // NOT inlined, and only here.
    //
    // This is the evaluator the RAY MARCH calls: once per step through
    // craycast, and four more times through cnormal. ctape_eval is a switch
    // over every primitive, combine op and deformer, so inlining it at each of
    // those sites makes clay_raycast enormous — and on an Apple Paravirtual
    // GPU the pipeline compiler gives up on it outright, with an error whose
    // entire content is "Compilation failed" (issue #63). The backend then
    // registers nothing and the library falls back to the CPU.
    //
    // clay_eval_points and clay_eval_grid call ctape_eval DIRECTLY and once,
    // where inlining is free, so they are untouched by this and still compile
    // to what they did. Nothing outside this file changes: the shared kernel
    // headers are what CPU, CUDA, OpenCL and Vulkan compile, and they see the
    // same source they always did.
    __attribute__((noinline)) float operator()(cfloat3 p) const {
        return ctape_eval(instrs, count, params, blob, p).d;
    }
};

}  // namespace

kernel void clay_eval_points(device const CTapeInstr* instrs [[buffer(0)]],
                             device const float* params [[buffer(1)]],
                             device const float* blob [[buffer(2)]],
                             device const float* pts [[buffer(3)]],
                             device float* out_d [[buffer(4)]],
                             device float* out_col [[buffer(5)]],
                             constant ClayEvalUniforms& u [[buffer(6)]],
                             uint gid [[thread_position_in_grid]]) {
    if (gid >= u.point_count) return;
    cfloat3 p = cf3(pts[gid * 3 + 0], pts[gid * 3 + 1], pts[gid * 3 + 2]);
    CTapeValue v = ctape_eval(instrs, (int)u.instr_count, params, blob, p);
    out_d[gid] = v.d;
    if (u.has_colors) {
        out_col[gid * 3 + 0] = v.color.x;
        out_col[gid * 3 + 1] = v.color.y;
        out_col[gid * 3 + 2] = v.color.z;
    }
}

kernel void clay_eval_grid(device const CTapeInstr* instrs [[buffer(0)]],
                           device const float* params [[buffer(1)]],
                           device const float* blob [[buffer(2)]],
                           device float* out_d [[buffer(3)]],
                           device float* out_col [[buffer(4)]],
                           constant ClayGridUniforms& u [[buffer(5)]],
                           uint gid [[thread_position_in_grid]]) {
    uint total = u.nx * u.ny * u.nz;
    if (gid >= total) return;
    uint x = gid % u.nx;
    uint y = (gid / u.nx) % u.ny;
    uint z = gid / (u.nx * u.ny);
    cfloat3 p = cf3(u.origin[0], u.origin[1], u.origin[2]) +
                cf3((float)x, (float)y, (float)z) * u.spacing;
    CTapeValue v = ctape_eval(instrs, (int)u.instr_count, params, blob, p);
    out_d[gid] = v.d;
    if (u.has_colors) {
        out_col[gid * 3 + 0] = v.color.x;
        out_col[gid * 3 + 1] = v.color.y;
        out_col[gid * 3 + 2] = v.color.z;
    }
}

kernel void clay_raycast(device const CTapeInstr* instrs [[buffer(0)]],
                         device const float* params [[buffer(1)]],
                         device const float* blob [[buffer(2)]],
                         device const float* rays [[buffer(3)]],
                         device ClayRayHitGpu* hits [[buffer(4)]],
                         constant ClayRayUniforms& u [[buffer(5)]],
                         uint gid [[thread_position_in_grid]]) {
    if (gid >= u.ray_count) return;
    cfloat3 ro = cf3(rays[gid * 6 + 0], rays[gid * 6 + 1], rays[gid * 6 + 2]);
    cfloat3 rd = cf3(rays[gid * 6 + 3], rays[gid * 6 + 4], rays[gid * 6 + 5]);

    ClayRayHitGpu hit;
    hit.hit = 0;
    hit.t = 0.0f;
    hit.pos[0] = hit.pos[1] = hit.pos[2] = 0.0f;
    hit.normal[0] = hit.normal[1] = hit.normal[2] = 0.0f;

    float tmin = u.tmin, tmax = u.tmax;
    if (u.has_bounds) {
        // slab clip against the scene bound (mirrors the CPU backend)
        cfloat3 bmin = cf3(u.bounds_min[0], u.bounds_min[1], u.bounds_min[2]);
        cfloat3 bmax = cf3(u.bounds_max[0], u.bounds_max[1], u.bounds_max[2]);
        float lo = tmin, hi = tmax;
        bool miss = false;
        for (int i = 0; i < 3; ++i) {
            float roi = i == 0 ? ro.x : (i == 1 ? ro.y : ro.z);
            float rdi = i == 0 ? rd.x : (i == 1 ? rd.y : rd.z);
            float mni = i == 0 ? bmin.x : (i == 1 ? bmin.y : bmin.z);
            float mxi = i == 0 ? bmax.x : (i == 1 ? bmax.y : bmax.z);
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

    TapeField field{instrs, (int)u.instr_count, params, blob};
    CRayHit r = craycast(field, ro, rd, tmin, tmax, u.eps, u.step_scale, 1.4f,
                         (int)u.max_steps);
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
