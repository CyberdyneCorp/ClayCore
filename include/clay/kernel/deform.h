#pragma once

// Deformers — metric breakers, all bound fields (docs/01 §2.5).
// Point-map deformers return the warped point at which to evaluate the
// wrapped field; weight helpers return spatial morph factors for the
// transition ops (the interpreter mixes the two subtree results).
// Every deformer's Lipschitz factor is provided by clay/kernel/exactness.h
// so consumers scale their steps; kernels here only warp.

#include "clay/kernel/shim.h"
#include "clay/kernel/ease.h"

CLAY_NS_BEGIN

// Twist around Y at k radians per unit height.
CLAY_FN cfloat3 ctwist_point(cfloat3 p, float k) {
    float c = ccos(k * p.y), s = csin(k * p.y);
    return cf3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
}

// Cheap bend along X at k radians per unit.
CLAY_FN cfloat3 cbend_point(cfloat3 p, float k) {
    float c = ccos(k * p.x), s = csin(k * p.x);
    return cf3(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
}

// Taper along Y between y0 and y1: cross-section scale goes s0 -> s1 with an
// easing curve. Returns the warped point; the field value must be multiplied
// by cmin(s0, s1) (conservative) by the interpreter.
CLAY_FN cfloat3 ctaper_point(cfloat3 p, float y0, float y1, float s0, float s1, int ease_type) {
    float t = cease(ease_type, cclamp((p.y - y0) / (y1 - y0), 0.0f, 1.0f));
    float s = cmix(s0, s1, t);
    return cf3(p.x / s, p.y, p.z / s);
}
CLAY_FN float ctaper_dist(float d, float s0, float s1) { return d * cmin(cmin(s0, s1), 1.0f); }

// Displacement: d' = d + g(p). g is evaluated by the interpreter (noise,
// texture, callable); the kernel just applies the sum.
CLAY_FN float cdisplace_dist(float d, float g) { return d + g; }

// bend_linear (fogleman): displace by v * ease(t) where t is the normalized
// projection of p onto segment a-b.
CLAY_FN cfloat3 cbend_linear_point(cfloat3 p, cfloat3 a, cfloat3 b, cfloat3 v, int ease_type) {
    cfloat3 ab = b - a;
    float t = cease(ease_type, cclamp(cdot(p - a, ab) / cdot(ab, ab), 0.0f, 1.0f));
    return p - v * t;
}

// bend_radial (fogleman): lift Y as a function of XZ radius, dz at r1.
CLAY_FN cfloat3 cbend_radial_point(cfloat3 p, float r0, float r1, float dz, int ease_type) {
    float r = clength(cf2(p.x, p.z));
    float t = cease(ease_type, cclamp((r - r0) / (r1 - r0), 0.0f, 1.0f));
    return cf3(p.x, p.y - dz * t, p.z);
}

// -- region-weighted warps (grab / pose) -------------------------------------
//
// These are the only deformers with FINITE SUPPORT: outside `radius` the map is
// exactly the identity, so an item's influence stays local and per-brick
// culling still holds. That is what lets them behave like a sculpting brush
// rather than a whole-item modifier.
//
// The weight is 1 at the centre and 0 at the rim, shaped by an easing curve.

CLAY_FN float cregion_weight(cfloat3 p, cfloat3 centre, float radius, int ease_type) {
    float d = clength(p - centre);
    return cease(ease_type, cclamp(1.0f - d / cmax(radius, 1e-6f), 0.0f, 1.0f));
}

// Half-space gate along the pull direction, so the far side of a form does not
// travel with the near side. The plane THROUGH the centre separates front from
// back; the transition is smoothed over half a radius either side rather than
// cut hard, because a discontinuity here would wreck the Lipschitz bound
// sphere tracing depends on. Half weight on the plane, nothing half a radius
// behind it.
CLAY_FN float cfront_gate(cfloat3 p, cfloat3 centre, float radius, cfloat3 dir) {
    float len = clength(dir);
    if (len < 1e-9f) return 1.0f;
    cfloat3 unit = dir * (1.0f / len);
    float band = cmax(radius * 0.5f, 1e-6f);
    return cclamp(cdot(p - centre, unit) / band * 0.5f + 0.5f, 0.0f, 1.0f);
}

// grab: translate a region by `displacement`, weighted from the centre out.
//
// Like every warp here the weight is evaluated at the SAMPLE point rather than
// at its preimage, which is what keeps the map closed-form. The consequence is
// that the surface travels less than the nominal displacement — pulling a unit
// sphere's tip with d=0.5 over r=0.8 moves it about 0.31, not 0.5. The pull is
// monotonic in |d|, so a UI can calibrate; solving for the true preimage would
// need an iteration per sample and buys nothing a sculptor can feel.
// front_only != 0 gates on the half-space the pull heads into.
CLAY_FN cfloat3 cgrab_point(cfloat3 p, cfloat3 centre, float radius, cfloat3 displacement,
                            float front_only, int ease_type) {
    float w = cregion_weight(p, centre, radius, ease_type);
    if (front_only != 0.0f) w = w * cfront_gate(p, centre, radius, displacement);
    return p - displacement * w;   // inverse map: sample where the material came from
}

// pose: rotate a region about `centre`, weighted the same way — the taper and
// repose motion, where a limb turns about a joint and the influence tapers off.
CLAY_FN cfloat3 cpose_point(cfloat3 p, cfloat3 centre, float radius, cfloat3 axis, float angle,
                            int ease_type) {
    float len = clength(axis);
    if (len < 1e-9f) return p;
    cfloat3 unit = axis * (1.0f / len);
    // Inverse map, so rotate by the negative of the weighted angle.
    float theta = -angle * cregion_weight(p, centre, radius, ease_type);
    cfloat3 v = p - centre;
    float s = csin(theta), c = ccos(theta);
    // Rodrigues
    cfloat3 rotated = v * c + ccross(unit, v) * s + unit * (cdot(unit, v) * (1.0f - c));
    return centre + rotated;
}

// wrap_around (fogleman): bend the X interval [x0, x1] around a cylinder
// (wraps text/reliefs around a column). Inverse map: unwrap the bent point
// back into flat space.
CLAY_FN cfloat3 cwrap_around_point(cfloat3 p, float x0, float x1) {
    float per = x1 - x0;
    float r = per / 6.2831853f;
    float a = catan2(p.y, p.x);
    float x = x0 + (a * 0.15915494f + 0.5f) * per;  // a/2pi
    return cf3(x, clength(cf2(p.x, p.y)) - r, p.z);
}

// Spatial morph weights: the interpreter evaluates both subtrees and mixes
// distances by w (a bound: lerped fields are not distances).
CLAY_FN float ctransition_linear_weight(cfloat3 p, cfloat3 a, cfloat3 b, int ease_type) {
    cfloat3 ab = b - a;
    return cease(ease_type, cclamp(cdot(p - a, ab) / cdot(ab, ab), 0.0f, 1.0f));
}
CLAY_FN float ctransition_radial_weight(cfloat3 p, float r0, float r1, int ease_type) {
    float r = clength(cf2(p.x, p.z));
    return cease(ease_type, cclamp((r - r0) / (r1 - r0), 0.0f, 1.0f));
}

CLAY_NS_END
