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
