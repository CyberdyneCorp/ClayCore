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
// [uniform scale s] [rounding r] [color rgb] [prim params, always
// CLAY_TAPE_PRIM_PARAMS wide] [deformer count] [deformer records...]. The
// inverse matrix already contains 1/s; the interpreter multiplies the local
// distance back by s and subtracts the rounding. Prim params are fixed-width
// so the deformer block sits at a known offset.
//
// Deformer record (CLAY_TAPE_DEFORM_FLOATS wide): [type] [k] [a] [b] [c]
// [ease]. Deformers warp the LOCAL point in authoring order before the
// primitive's distance function; each may also correct the distance after.
//
// Combine param block: [mode] [profile] [k] [r2], followed for transition
// modes by their own parameters — linear: [ax ay az bx by bz ease],
// radial: [r0 r1 ease]. Every other mode reads only the first four. r2 is the second radius
// of the two-parameter extended modes (groove/tongue half-width rb, mapped
// from the item's rounding in world units by the compiler); 0 elsewhere.

#include "clay/kernel/deform.h"
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

    // Pushes the far field ("empty space"): standard primitive param block,
    // no prim-specific params. The compiler emits it to seed material-
    // creating combines (shell, replace) whose accumulator is absent.
    ctape_empty = 32,

    ctape_combine = 64,
};

// Extended modes (ops.h math; a = accumulated field, b = item field).
// Parameter mapping: k is the mode's radius/depth (pipe radius, engrave/
// emboss depth, groove/tongue depth ra, inset depth, shell half-thickness);
// r2 is the groove/tongue half-width rb (compiled from the item's rounding
// in world units). Color semantics: carving modes (groove, engrave, inset)
// keep a's color; adding modes (tongue, pipe, emboss, shell) take b's color
// where the added field wins; replace takes b's color inside b.
enum CCombineMode {
    ccombine_add = 0,
    ccombine_subtract = 1,
    ccombine_intersect = 2,
    ccombine_paint = 3,  // color-only (stain)
    ccombine_groove = 4,
    ccombine_tongue = 5,
    ccombine_pipe = 6,
    ccombine_engrave = 7,  // deboss
    ccombine_emboss = 8,
    ccombine_inset = 9,
    ccombine_shell = 10,
    ccombine_replace = 11,
    // Spatial morphs between the accumulated field and the item
    // (deform.h). NON-LOCAL: the weight is non-zero arbitrarily far from
    // both operands, so the scene marks these items infinite-influence and
    // never culls them. Extra params follow the shared four (see below).
    ccombine_transition_linear = 12,
    ccombine_transition_radial = 13,
};

enum CBlendProfile {
    cblend_hard = 0,
    cblend_quadratic = 1,
    cblend_cubic = 2,
    cblend_circular = 3,
    cblend_chamfer = 4,
};

// typedef form: the OpenCL backend compiles these headers as C99, where a
// bare struct tag is not a type name.
typedef struct CTapeInstrT {
    unsigned int op;
    unsigned int param_offset;
} CTapeInstr;

typedef struct CTapeValueT {
    float d;
    cfloat3 color;
} CTapeValue;

#define CLAY_TAPE_MAX_STACK 16
#define CLAY_TAPE_FAR 3.4e37f
#define CLAY_TAPE_PRIM_PARAMS 7
#define CLAY_TAPE_DEFORM_FLOATS 6
#define CLAY_TAPE_PRIM_HEADER 17

// Domain warps an item can carry (deform.h). Values are serialization-stable.
enum CDeformType {
    cdeform_twist = 0,     // k = radians per unit about Y
    cdeform_bend = 1,      // k = radians per unit along X
    cdeform_taper = 2,     // k = y0, a = y1, b = s0, c = s1, ease
    cdeform_displace = 3,  // k = amplitude, a = frequency
};

// Apply one deformer record to the local point. No deformer corrects the
// distance: safety is carried by the tape's tracked Lipschitz factor, which
// also keeps influence bounds tight (see the taper note below).
CLAY_FN cfloat3 ctape_deform_point(CLAY_DEVICE const float* rec, cfloat3 p) {
    int type = (int)rec[0];
    if (type == cdeform_twist) return ctwist_point(p, rec[1]);
    if (type == cdeform_bend) return cbend_point(p, rec[1]);
    if (type == cdeform_taper) {
        // NOTE: deliberately no ctaper_dist here. Multiplying the distance by
        // min(s,1) would keep the field conservative on its own, but it also
        // shrinks the field everywhere, which makes the item's influence
        // reach 1/s further than its geometry — and influence bounds are what
        // brick culling trusts. Instead the taper behaves like twist and bend:
        // the raw warped field, with safety carried by the tape's tracked
        // Lipschitz factor (cfi_taper includes the 1/s_min stretch).
        return ctaper_point(p, rec[1], rec[2], rec[3], rec[4], (int)rec[5]);
    }
    return p;  // displace acts on the distance, not the point
}

// Post-primitive distance contribution of one deformer (0 for point warps).
CLAY_FN float ctape_deform_offset(CLAY_DEVICE const float* rec, cfloat3 p) {
    if ((int)rec[0] != cdeform_displace) return 0.0f;
    float amp = rec[1];
    float freq = rec[2];
    return amp * csin(freq * p.x) * csin(freq * p.y) * csin(freq * p.z);
}

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

// Support width of an extended combine mode: the band-clamped result can
// differ from `a` only where the item field b < support + band, so influence
// bounds dilate the item bound by this width. Groove/tongue deviate within
// the channel half-width r2 of b's surface; pipe within its radius k of the
// intersection curve; engrave/emboss within the V depth k; shell within the
// wall half-thickness k. Inset and replace are decided by b's own sign
// outside its bound (support 0). The blend profile is ignored by these
// modes.
CLAY_FN float ccombine_extended_support(int mode, float k, float r2) {
    if (mode == ccombine_groove || mode == ccombine_tongue) return cmax(r2, 0.0f);
    if (mode == ccombine_inset || mode == ccombine_replace) return 0.0f;
    return cmax(k, 0.0f);
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

// Transition weight for the morph modes (0 -> accumulated, 1 -> item).
CLAY_FN float ctape_transition_weight(int mode, CLAY_DEVICE const float* extra, cfloat3 p) {
    if (mode == ccombine_transition_linear) {
        return ctransition_linear_weight(p, cf3(extra[0], extra[1], extra[2]),
                                         cf3(extra[3], extra[4], extra[5]), (int)extra[6]);
    }
    return ctransition_radial_weight(p, extra[0], extra[1], (int)extra[2]);
}

CLAY_FN bool ctape_mode_is_transition(int mode) {
    return mode == ccombine_transition_linear || mode == ccombine_transition_radial;
}

CLAY_FN CTapeValue ctape_combine_values(CTapeValue a, CTapeValue b, int mode, int profile,
                                        float k, float r2) {
    CTapeValue r;
    r.d = a.d;
    r.color = a.color;
    if (mode == ccombine_add) {
        cfloat2 dm = ctape_smin_m(profile, a.d, b.d, k);
        r.d = dm.x;
        r.color = cmix(a.color, b.color, dm.y);
    } else if (mode == ccombine_subtract) {
        // b carves out of a: -smin(-a, b)
        r.d = -ctape_smin(profile, -a.d, b.d, k);
    } else if (mode == ccombine_intersect) {
        r.d = -ctape_smin(profile, -a.d, -b.d, k);
    } else if (mode == ccombine_paint) {
        // color-only, field untouched
        float support = cmax(ctape_blend_support(profile, k), k);
        float w = 1.0f - cclamp(b.d / cmax(support, 1e-6f), 0.0f, 1.0f);
        r.color = cmix(a.color, b.color, w);
    } else if (mode == ccombine_groove) {
        r.d = op_groove(a.d, b.d, k, r2);
    } else if (mode == ccombine_tongue) {
        r.d = op_tongue(a.d, b.d, k, r2);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_pipe) {
        r.d = op_pipe(a.d, b.d, k);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_engrave) {
        r.d = op_engrave(a.d, b.d, k);
    } else if (mode == ccombine_emboss) {
        r.d = op_emboss(a.d, b.d, k);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_inset) {
        r.d = op_inset(a.d, b.d, k);
    } else if (mode == ccombine_shell) {
        r.d = op_shell_union(a.d, b.d, k);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_replace) {
        r.d = op_replace(a.d, b.d);
        if (b.d < 0.0f) r.color = b.color;
    }
    // unknown modes fall through as identity (forward compatibility)
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
            if (top < 1) continue;
            CTapeValue b = stack[top - 1];
            CTapeValue a;
            // A combine with an empty accumulator applies against empty
            // space: this is how material-creating modes (shell, replace)
            // seed a chain whose earlier items were culled or absent.
            a.d = CLAY_TAPE_FAR;
            a.color = b.color;
            if (top >= 2) {
                a = stack[top - 2];
                --top;
            }
            int mode = (int)pr[0];
            if (ctape_mode_is_transition(mode)) {
                // spatial morph: lerp BOTH operands by the weight at p. Not a
                // distance — the tape's tracked field info records that.
                float w = ctape_transition_weight(mode, pr + 4, p);
                CTapeValue r;
                r.d = cmix(a.d, b.d, w);
                r.color = cmix(a.color, b.color, w);
                stack[top - 1] = r;
            } else {
                stack[top - 1] = ctape_combine_values(a, b, mode, (int)pr[1], pr[2], pr[3]);
            }
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
            // deformer chain: warp the local point in authoring order, then
            // add each deformer's distance contribution (deform.h semantics)
            CLAY_DEVICE const float* deform = pr + CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS;
            int deform_count = (int)deform[0];
            float offset = 0.0f;
            cfloat3 wp = lp;
            for (int di = 0; di < deform_count; ++di) {
                CLAY_DEVICE const float* rec = deform + 1 + di * CLAY_TAPE_DEFORM_FLOATS;
                offset += ctape_deform_offset(rec, wp);
                wp = ctape_deform_point(rec, wp);
            }
            v.d = (ctape_prim_dist(op, pr + CLAY_TAPE_PRIM_HEADER, strokes, wp) + offset) *
                      scale - round;
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
