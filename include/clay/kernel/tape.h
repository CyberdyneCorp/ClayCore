#pragma once

// Flat postfix tape: the ONE program format every backend interprets
// (scene-model spec: scenes never become shader code; parameter edits only
// rewrite param blocks). The scene module compiles edit lists into this
// format (clay/scene/tape_build.h); this header owns the opcode set, the
// binary layout, and the fixed interpreter, all in the kernel dialect.
//
// Layout
//   instrs:  CTapeInstr[] — {op, param_offset into params[]}
//   params:  float[] — per-instruction parameter blocks
//   strokes: float[] — stroke point data, 4 floats per point (x, y, z, r)
//
// Primitive param block: [inv affine 12 floats (columns c0..c3, xyz each)]
// [uniform scale s] [rounding r] [color rgb] [prim-specific...]. The inverse
// matrix already contains 1/s; the interpreter multiplies the local distance
// back by s and subtracts the rounding.
//
// Combine param block: [mode] [profile] [k].

#include "clay/kernel/ops.h"
#include "clay/kernel/prim3d.h"
#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

enum CTapeOp {
    ctape_sphere = 0,
    ctape_box,
    ctape_round_box,
    ctape_box_frame,
    ctape_torus,
    ctape_capsule,           // local endpoints a, b + radius
    ctape_capped_cylinder,   // r, h
    ctape_rounded_cylinder,  // ra, rb, h
    ctape_capped_cone,       // h, r1, r2
    ctape_round_cone,        // r1, r2, h
    ctape_ellipsoid,         // rx, ry, rz (bound)
    ctape_octahedron,        // s
    ctape_hex_prism,         // hx, hy
    ctape_pyramid,           // h
    ctape_stroke,            // blend_k, point_offset, point_count
    ctape_prim_count,

    ctape_combine = 64,
};

enum CCombineMode {
    ccombine_add = 0,
    ccombine_subtract = 1,
    ccombine_intersect = 2,
    ccombine_paint = 3,  // color-only
};

enum CBlendProfile {
    cblend_hard = 0,
    cblend_quadratic = 1,
    cblend_cubic = 2,
    cblend_circular = 3,
    cblend_chamfer = 4,
};

struct CTapeInstr {
    unsigned int op;
    unsigned int param_offset;
};

struct CTapeValue {
    float d;
    cfloat3 color;
};

#define CLAY_TAPE_MAX_STACK 16
#define CLAY_TAPE_FAR 3.4e37f

// Blend support width in world units — deviation from the hard op is zero
// beyond this |a-b| difference. The scene compiler uses the same function
// for influence bounds.
CLAY_FN float ctape_blend_support(int profile, float k) {
    if (profile == cblend_quadratic) return csmin_quadratic_support(k);
    if (profile == cblend_cubic) return csmin_cubic_support(k);
    if (profile == cblend_circular) return csmin_circular_support(k);
    if (profile == cblend_chamfer) return cchamfer_support(k);
    return 0.0f;
}

CLAY_FN cfloat2 ctape_smin_m(int profile, float a, float b, float k) {
    if (profile == cblend_quadratic) return csmin_quadratic_m(a, b, k);
    if (profile == cblend_cubic) return csmin_cubic_m(a, b, k);
    if (profile == cblend_circular) return csmin_circular_m(a, b, k);
    if (profile == cblend_chamfer) return cchamfer_m(a, b, k);
    return (a < b) ? cf2(a, 0.0f) : cf2(b, 1.0f);
}

CLAY_FN float ctape_smin(int profile, float a, float b, float k) {
    if (profile == cblend_quadratic) return csmin_quadratic(a, b, k);
    if (profile == cblend_cubic) return csmin_cubic(a, b, k);
    if (profile == cblend_circular) return csmin_circular(a, b, k);
    if (profile == cblend_chamfer) return cchamfer(a, b, k);
    return cmin(a, b);
}

// Stroke: chain of sphere-swept segments over raw float data (4 per point).
CLAY_FN float ctape_stroke_dist(CLAY_DEVICE const float* pts, int count, cfloat3 p,
                                float blend_k) {
    if (count <= 0) return CLAY_TAPE_FAR;
    cfloat3 a0 = cf3(pts[0], pts[1], pts[2]);
    if (count == 1) return sd_sphere(p - a0, pts[3]);
    float d = CLAY_TAPE_FAR;
    for (int i = 0; i + 1 < count; ++i) {
        cfloat3 a = cf3(pts[i * 4 + 0], pts[i * 4 + 1], pts[i * 4 + 2]);
        cfloat3 b = cf3(pts[i * 4 + 4], pts[i * 4 + 5], pts[i * 4 + 6]);
        float ra = pts[i * 4 + 3];
        float rb = pts[i * 4 + 7];
        float seg;
        if (cdot2(b - a) < 1e-12f) {
            seg = sd_sphere(p - a, cmax(ra, rb));
        } else if (cabs(ra - rb) < 1e-7f) {
            seg = sd_capsule(p, a, b, ra);
        } else {
            seg = sd_round_cone_ab(p, a, b, ra, rb);
        }
        d = (blend_k > 0.0f) ? csmin_quadratic(d, seg, blend_k) : cmin(d, seg);
    }
    return d;
}

CLAY_FN float ctape_prim_dist(unsigned int op, CLAY_DEVICE const float* q,
                              CLAY_DEVICE const float* strokes, cfloat3 lp) {
    // q points at the prim-specific params (after xform/scale/round/color).
    if (op == ctape_sphere) return sd_sphere(lp, q[0]);
    if (op == ctape_box) return sd_box(lp, cf3(q[0], q[1], q[2]));
    if (op == ctape_round_box) return sd_round_box(lp, cf3(q[0], q[1], q[2]), q[3]);
    if (op == ctape_box_frame) return sd_box_frame(lp, cf3(q[0], q[1], q[2]), q[3]);
    if (op == ctape_torus) return sd_torus(lp, q[0], q[1]);
    if (op == ctape_capsule)
        return sd_capsule(lp, cf3(q[0], q[1], q[2]), cf3(q[3], q[4], q[5]), q[6]);
    if (op == ctape_capped_cylinder) return sd_capped_cylinder(lp, q[0], q[1]);
    if (op == ctape_rounded_cylinder) return sd_rounded_cylinder(lp, q[0], q[1], q[2]);
    if (op == ctape_capped_cone) return sd_capped_cone(lp, q[0], q[1], q[2]);
    if (op == ctape_round_cone) return sd_round_cone(lp, q[0], q[1], q[2]);
    if (op == ctape_ellipsoid) return sd_ellipsoid_bound(lp, cf3(q[0], q[1], q[2]));
    if (op == ctape_octahedron) return sd_octahedron(lp, q[0]);
    if (op == ctape_hex_prism) return sd_hex_prism(lp, cf2(q[0], q[1]));
    if (op == ctape_pyramid) return sd_pyramid(lp, q[0]);
    if (op == ctape_stroke) {
        int off = (int)q[1];
        int cnt = (int)q[2];
        return ctape_stroke_dist(strokes + off, cnt, lp, q[0]);
    }
    return CLAY_TAPE_FAR;
}

CLAY_FN CTapeValue ctape_combine_values(CTapeValue a, CTapeValue b, int mode, int profile, float k) {
    CTapeValue r;
    if (mode == ccombine_add) {
        cfloat2 dm = ctape_smin_m(profile, a.d, b.d, k);
        r.d = dm.x;
        r.color = cmix(a.color, b.color, dm.y);
    } else if (mode == ccombine_subtract) {
        // b carves out of a: -smin(-a, b)
        r.d = -ctape_smin(profile, -a.d, b.d, k);
        r.color = a.color;
    } else if (mode == ccombine_intersect) {
        r.d = -ctape_smin(profile, -a.d, -b.d, k);
        r.color = a.color;
    } else {  // paint: color-only, field untouched
        float support = cmax(ctape_blend_support(profile, k), k);
        float w = 1.0f - cclamp(b.d / cmax(support, 1e-6f), 0.0f, 1.0f);
        r.d = a.d;
        r.color = cmix(a.color, b.color, w);
    }
    return r;
}

// The fixed interpreter every backend ships. Postfix stack machine; empty
// tapes evaluate to "far outside".
CLAY_FN CTapeValue ctape_eval(CLAY_DEVICE const CTapeInstr* instrs, int instr_count,
                              CLAY_DEVICE const float* params, CLAY_DEVICE const float* strokes,
                              cfloat3 p) {
    CTapeValue stack[CLAY_TAPE_MAX_STACK];
    int top = 0;
    for (int i = 0; i < instr_count; ++i) {
        unsigned int op = instrs[i].op;
        CLAY_DEVICE const float* pr = params + instrs[i].param_offset;
        if (op == ctape_combine) {
            if (top < 2) continue;
            CTapeValue b = stack[top - 1];
            CTapeValue a = stack[top - 2];
            stack[top - 2] = ctape_combine_values(a, b, (int)pr[0], (int)pr[1], pr[2]);
            --top;
        } else {
            if (top >= CLAY_TAPE_MAX_STACK) continue;
            cfloat4x4 inv;
            inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
            inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
            inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
            inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
            float scale = pr[12];
            float round = pr[13];
            CTapeValue v;
            v.color = cf3(pr[14], pr[15], pr[16]);
            cfloat3 lp = cmul_point(inv, p);
            v.d = ctape_prim_dist(op, pr + 17, strokes, lp) * scale - round;
            stack[top] = v;
            ++top;
        }
    }
    if (top == 0) {
        CTapeValue far_out;
        far_out.d = CLAY_TAPE_FAR;
        far_out.color = cf3(0.5f, 0.5f, 0.5f);
        return far_out;
    }
    return stack[top - 1];
}

CLAY_NS_END
