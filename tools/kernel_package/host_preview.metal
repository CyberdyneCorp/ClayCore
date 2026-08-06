// Worked example: a host evaluating a ClayCore tape on its own GPU.
//
// This file is what the published kernels artifact is FOR. It contains no
// distance function, no blend and no deformer of its own — every one of those
// comes from clay/kernel/*.h, the same headers the CPU, Metal, CUDA and
// OpenCL backends compile. A preview built this way cannot drift from the
// bake, because there is nothing left to drift.
//
// Build it with the packaged headers on the Metal header search path:
//
//   xcrun -sdk macosx metal -c host_preview.metal \
//         -I <claycore-kernels>/include -o host_preview.air
//
// No -D flags: shim.h reads __METAL_VERSION__ and selects the MSL branch.
//
// The tape buffers (instrs / params / blob) are what ClayCore compiles a
// document into; feed the same three buffers here and to `clay_eval_points`
// and the two agree by construction. `clay parity-fixture` exports tapes,
// probe points and reference values so a host can assert exactly that in its
// own CI — see docs/06-host-gpu-previews.md.

#include "clay/kernel/kernels.h"

using namespace clay::kernels;  // MSL reserves `kernel`; see shim.h

struct PreviewUniforms {
    uint instr_count;
    uint point_count;
};

// Field functor over device tape buffers, for the templated utilities in
// field.h (normals, sphere tracing, AO).
struct TapeField {
    device const CTapeInstr* instrs;
    int count;
    device const float* params;
    device const float* blob;
    float operator()(cfloat3 p) const { return ctape_eval(instrs, count, params, blob, p).d; }
};

// Batch field query — the host analogue of clay_eval_points.
kernel void clay_host_preview_eval(device const CTapeInstr* instrs [[buffer(0)]],
                                   device const float* params [[buffer(1)]],
                                   device const float* blob [[buffer(2)]],
                                   device const float* points [[buffer(3)]],
                                   device float* out_distance [[buffer(4)]],
                                   device float* out_color [[buffer(5)]],
                                   constant PreviewUniforms& u [[buffer(6)]],
                                   uint gid [[thread_position_in_grid]]) {
    if (gid >= u.point_count) return;
    cfloat3 p = cf3(points[gid * 3 + 0], points[gid * 3 + 1], points[gid * 3 + 2]);
    CTapeValue v = ctape_eval(instrs, (int)u.instr_count, params, blob, p);
    out_distance[gid] = v.d;
    out_color[gid * 3 + 0] = v.color.x;
    out_color[gid * 3 + 1] = v.color.y;
    out_color[gid * 3 + 2] = v.color.z;
}

// Surface normal at a point, for shading the preview. Same tetrahedron trick
// the CPU backend uses, from the same header.
kernel void clay_host_preview_normal(device const CTapeInstr* instrs [[buffer(0)]],
                                     device const float* params [[buffer(1)]],
                                     device const float* blob [[buffer(2)]],
                                     device const float* points [[buffer(3)]],
                                     device float* out_normal [[buffer(4)]],
                                     constant PreviewUniforms& u [[buffer(5)]],
                                     uint gid [[thread_position_in_grid]]) {
    if (gid >= u.point_count) return;
    TapeField field{instrs, (int)u.instr_count, params, blob};
    cfloat3 p = cf3(points[gid * 3 + 0], points[gid * 3 + 1], points[gid * 3 + 2]);
    cfloat3 n = cnormal(field, p, 1e-4f);
    out_normal[gid * 3 + 0] = n.x;
    out_normal[gid * 3 + 1] = n.y;
    out_normal[gid * 3 + 2] = n.z;
}
