#pragma once

// Field classification and safe-step bookkeeping (docs/01 §2.7).
//
// Every node in an SDF expression tree carries a CFieldInfo:
//   is_exact  — true Euclidean distance (|grad f| = 1 a.e.)
//   lipschitz — upper bound on |grad f|; stepping by f(p)/L never crosses
//               the surface. Underestimating bounds (smins, ellipsoid, ...)
//               keep L = 1: they are safe at full steps, just not exact.
//
// The scene's tape compiler folds these combinators over the tree and
// stores the resulting safe step scale; no consumer may assume |grad f| = 1
// unless the tree proves exactness (sdf-kernels spec).

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

struct CFieldInfo {
    bool is_exact;
    float lipschitz;
};

CLAY_FN float csafe_step_scale(CFieldInfo i) { return 1.0f / cmax(i.lipschitz, 1.0f); }

CLAY_FN CFieldInfo cfi_exact() { return CFieldInfo{true, 1.0f}; }
CLAY_FN CFieldInfo cfi_bound() { return CFieldInfo{false, 1.0f}; }

// Hard CSG: preserves exactness in the safe-step sense (interior-seam caveat
// of docs/01 §2.1 does not affect stepping).
CLAY_FN CFieldInfo cfi_boolean(CFieldInfo a, CFieldInfo b) {
    return CFieldInfo{a.is_exact && b.is_exact, cmax(a.lipschitz, b.lipschitz)};
}

// Smooth blends: |grad| < 1 inside the blend zone — a bound, still safe.
CLAY_FN CFieldInfo cfi_smooth_blend(CFieldInfo a, CFieldInfo b) {
    return CFieldInfo{false, cmax(a.lipschitz, b.lipschitz)};
}

// Extended blends (ops.h). Modes built from min/max/abs of the operands
// (groove, tongue, inset, shell, replace) preserve the Lipschitz bound.
// The diagonal 45-degree constructions — pipe, engrave, emboss — mix both
// gradients: |grad| can reach (La + Lb) / sqrt(2), so pass diagonal = true.
CLAY_FN CFieldInfo cfi_extended_blend(CFieldInfo a, CFieldInfo b, bool diagonal) {
    float l = cmax(a.lipschitz, b.lipschitz);
    if (diagonal) l = cmax(l, (a.lipschitz + b.lipschitz) * 0.70710678f);
    return CFieldInfo{false, l};
}

// Rigid transform, uniform scale, round/onion, elongate, extrude, revolve,
// repetition with adequate padding: exactness and Lipschitz preserved.
CLAY_FN CFieldInfo cfi_isometric(CFieldInfo a) { return a; }

// Non-uniform scale via cscale_nu_*: conservative multiply keeps L, loses
// exactness.
CLAY_FN CFieldInfo cfi_scale_nonuniform(CFieldInfo a) {
    return CFieldInfo{false, a.lipschitz};
}

// Twist at k rad/unit applied to content within XZ radius r of the axis.
CLAY_FN CFieldInfo cfi_twist(CFieldInfo a, float k, float radius_bound) {
    return CFieldInfo{false, a.lipschitz * (1.0f + cabs(k) * radius_bound)};
}

// Bend at k rad/unit applied to content within distance r of the bend axis.
CLAY_FN CFieldInfo cfi_bend(CFieldInfo a, float k, float radius_bound) {
    return CFieldInfo{false, a.lipschitz * (1.0f + cabs(k) * radius_bound)};
}

// Taper with cross-section scales in [s_min, s_max] over height h and
// content radius r: the warp stretches by at most max(1/s_min, 1 + r·|Δs|/h).
CLAY_FN CFieldInfo cfi_taper(CFieldInfo a, float s_min, float s_max, float height,
                             float radius_bound) {
    float stretch = cmax(1.0f / cmax(s_min, 1e-6f),
                         1.0f + radius_bound * cabs(s_max - s_min) / cmax(height, 1e-6f));
    return CFieldInfo{false, a.lipschitz * stretch};
}

// Displacement d + g with Lipschitz(g) = lg: L' = L + lg (docs/01 §2.5).
CLAY_FN CFieldInfo cfi_displace(CFieldInfo a, float lg) {
    return CFieldInfo{false, a.lipschitz + lg};
}

// bend_linear displacing by vector v over segment length len: worst-case
// warp gradient grows by |v|/len.
CLAY_FN CFieldInfo cfi_bend_linear(CFieldInfo a, float v_len, float seg_len) {
    return CFieldInfo{false, a.lipschitz * (1.0f + v_len / cmax(seg_len, 1e-6f))};
}

// bend_radial lifting by dz over radial span (r1 - r0).
CLAY_FN CFieldInfo cfi_bend_radial(CFieldInfo a, float dz, float r0, float r1) {
    return CFieldInfo{false, a.lipschitz * (1.0f + cabs(dz) / cmax(cabs(r1 - r0), 1e-6f))};
}

// wrap_around of interval [x0, x1]: stretch grows with radial distance from
// the wrap cylinder; content thickness t (radial extent) bounds it.
CLAY_FN CFieldInfo cfi_wrap_around(CFieldInfo a, float x0, float x1, float thickness) {
    float r = cabs(x1 - x0) * 0.15915494f;  // per / 2pi
    return CFieldInfo{false, a.lipschitz * (1.0f + thickness / cmax(r, 1e-6f))};
}

// grab: the displacement is ramped over the radius, so the stretch is the
// displacement magnitude against the distance it ramps across, scaled by the
// easing curve's steepest slope. The front-facing gate adds a second ramp of
// its own across half a radius, so it contributes a slope of 2.
CLAY_FN CFieldInfo cfi_grab(CFieldInfo a, float displacement_len, float radius, float ease_slope,
                            bool front_gated) {
    float slope = ease_slope + (front_gated ? 2.0f : 0.0f);
    return CFieldInfo{false, a.lipschitz * (1.0f + displacement_len * slope / cmax(radius, 1e-6f))};
}

// pose: a point at distance d from the centre sweeps an arc of d * dtheta, and
// d is at most the radius inside the support, so the radius cancels and the
// stretch is bounded by the angle against the easing curve's slope.
CLAY_FN CFieldInfo cfi_pose(CFieldInfo a, float angle, float ease_slope) {
    return CFieldInfo{false, a.lipschitz * (1.0f + cabs(angle) * ease_slope)};
}

// transition (spatial morph of two subtrees): lerped fields are not
// distances; the mix adds |d1 - d2| · Lipschitz(w). The compiler passes a
// bound on the field difference over the influence region.
CLAY_FN CFieldInfo cfi_transition(CFieldInfo a, CFieldInfo b, float diff_bound, float span) {
    float l = cmax(a.lipschitz, b.lipschitz) + diff_bound / cmax(span, 1e-6f);
    return CFieldInfo{false, l};
}

CLAY_NS_END
