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

// noise: offsetting the distance raises the slope by the offset's gradient, as
// displace does — but summed over the OCTAVES, because each octave has twice
// the frequency of the last and so twice the gradient for the same amplitude.
//
// With the fractal normalized by its total weight, octave i contributes
// amplitude * frequency * (2*gain)^i / sum(gain^j). At the usual gain of 0.5
// that is flat across octaves, so the count multiplies rather than converges —
// which is exactly why a caller who raises octaves finds the marcher slowing.
// kGradient is the steepest a unit-amplitude gradient noise gets.
CLAY_FN CFieldInfo cfi_noise(CFieldInfo a, float amplitude, float frequency, int octaves,
                             float gain) {
    const float kGradient = 2.0f;
    float weight = 0.0f;
    float slope = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        weight = weight + amp;
        slope = slope + amp * freq;
        amp = amp * gain;
        freq = freq * 2.0f;
    }
    float g = weight > 0.0f ? slope / weight : 0.0f;
    return CFieldInfo{false, a.lipschitz + cabs(amplitude) * cabs(frequency) * g * kGradient};
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

// magnify / pinch: a radial scale is not distance preserving, and it stretches
// most where the falloff is steepest. Along the radius the map is r -> r * s(r)
// with s = 1 - strength * w, so the derivative is s + r * ds/dr, and ds/dr is
// bounded by |strength| * (the easing curve's slope) / radius. With r at most
// the radius inside the support, the radius cancels and what is left is the
// strength against that slope — the same shape cfi_pose ends up with, for the
// same reason.
//
// Both signs cost: magnifying stretches space and pinching compresses it, and
// the marcher has to survive either.
CLAY_FN CFieldInfo cfi_magnify(CFieldInfo a, float strength, float ease_slope) {
    return CFieldInfo{false, a.lipschitz * (1.0f + cabs(strength) * (1.0f + ease_slope))};
}

// pose along a line: the angle ramps over the segment, so a point at distance
// `extent` from the axis is dragged tangentially by extent * dtheta/ds. The
// ramp runs over the segment length, hence the division by it.
CLAY_FN CFieldInfo cfi_pose_line(CFieldInfo a, float angle, float extent, float segment_len,
                                 float ease_slope) {
    return CFieldInfo{false, a.lipschitz * (1.0f + cabs(angle) * extent * ease_slope /
                                                       cmax(segment_len, 1e-6f))};
}

// transition (spatial morph of two subtrees): lerped fields are not
// distances; the mix adds |d1 - d2| · Lipschitz(w). The compiler passes a
// bound on the field difference over the influence region.
CLAY_FN CFieldInfo cfi_transition(CFieldInfo a, CFieldInfo b, float diff_bound, float span) {
    float l = cmax(a.lipschitz, b.lipschitz) + diff_bound / cmax(span, 1e-6f);
    return CFieldInfo{false, l};
}

// loft: interpolating two profile fields along the lift axis is the same
// situation cfi_transition describes, in one dimension instead of three. The
// mix adds |da - db| / (depth it is mixed across), and an easing curve
// steepens that by its own maximum slope. Reporting Lipschitz 1 here would
// let the raymarcher step as if the field were a distance and walk through
// the surface.
// a sampled volume. Trilinear interpolation of a 1-Lipschitz field: adjacent
// samples differ by at most the cell size, so each partial derivative of the
// interpolant is bounded by 1 and the gradient magnitude by sqrt(3). The bound
// is tight — a corner configuration reaches it.
//
// Declaring 1.0 here would not be a rounding choice, it would be wrong: the
// marcher would take a full step through a region where the interpolant is
// steeper than the field it was built from, which is exactly the overstep the
// sparse index is careful to avoid everywhere else.
// `sample_lipschitz` is how fast the STORED SAMPLES may vary, which is 1 for a
// volume sampled from a distance field but more for one an operator has
// steepened. A brush is the usual way that happens: any effect confined to a
// region blends under a weight that varies across it, and that adds a term
// proportional to how far the value moves times the gradient of the weight.
// An operator that does so declares it here rather than letting the marcher
// find out.
// relief: the accumulated field is offset by amplitude * w(b.d), and w varies
// over the falloff width, so the slope gains |amplitude| / width times the
// weight curve's own steepest slope. A smoothstep peaks at 1.5.
//
// That ratio is the whole cost model: a deep relief through a narrow falloff is
// a steep field, and the marcher pays for it by a declared amount rather than
// by stepping through the surface.
CLAY_FN CFieldInfo cfi_relief(CFieldInfo a, float amplitude, float width) {
    return CFieldInfo{false, a.lipschitz + cabs(amplitude) * 1.5f / cmax(width, 1e-6f)};
}

CLAY_FN CFieldInfo cfi_volume(float sample_lipschitz) {
    return CFieldInfo{false, 1.7320508f * cmax(sample_lipschitz, 1.0f)};
}

// Feathered replace of a sampled volume: a crossfade adds the blended
// difference times the weight's own gradient, the same shape cfi_relief and
// cfi_transition pay for. The difference is CLAMPED at the volume's band in
// the kernel (ctape_replace_feather), so unlike a transition the bound here
// is structural rather than a region diagonal: band * 1.5 / feather, with
// 1.5 the smoothstep's peak slope. Both lengths are in the same units, so
// the instance scale cancels.
CLAY_FN CFieldInfo cfi_replace_feather(CFieldInfo a, CFieldInfo b, float band, float feather) {
    return CFieldInfo{false,
                      cmax(a.lipschitz, b.lipschitz) + band * 1.5f / cmax(feather, 1e-6f)};
}

CLAY_FN CFieldInfo cfi_loft(float profile_spread, float depth, float ease_slope) {
    return CFieldInfo{false, 1.0f + profile_spread * ease_slope / cmax(depth, 1e-6f)};
}

// swept along a guide. TWO terms, and leaving either out understates the
// field's steepness:
//
//   curvature — the sweep compresses space on the inside of a bend, so a point
//   at perpendicular offset r inside a bend of radius R is squeezed by
//   R / (R - r). Same shape as cfi_wrap_around, which is the same geometry.
//
//   interpolation — the profiles are lerped along the guide exactly as a
//   loft's are along Z, so cfi_loft's term applies over the arc length. A
//   straight guide has no curvature at all, and a sweep that reported only the
//   curvature term would claim Lipschitz 1 for a tapering tube — the precise
//   defect this file exists to prevent.
//
// When the widest profile reaches the tightest bend radius the sweep folds
// through itself. That is NOT refused — a guide is editable after the fact, so
// it has to degrade rather than fail — and the factor is clamped to something
// large, which makes the raymarcher crawl instead of stepping through a
// surface it was told was a distance field.
CLAY_FN CFieldInfo cfi_swept(float profile_extent, float bend_radius, float profile_spread,
                             float span_len, float ease_slope) {
    float lerp = 1.0f + profile_spread * ease_slope / cmax(span_len, 1e-6f);
    float headroom = bend_radius - profile_extent;
    if (headroom <= 1e-4f) return CFieldInfo{false, 1e4f};
    return CFieldInfo{false, (bend_radius / headroom) * lerp};
}

CLAY_NS_END
