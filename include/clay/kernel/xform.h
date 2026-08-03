#pragma once

// Transforms, symmetry, and structure operators (docs/01 §2.3).
// Point-map operators return the point at which to evaluate the wrapped
// field; distance-map operators post-process the wrapped field's value.
// The tape interpreter composes them; exactness bookkeeping lives in
// clay/kernel/exactness.h.

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

// Rigid transform: evaluate the primitive at inv * p (transforms are
// pre-inverted at tape-compile time). Exact.
CLAY_FN cfloat3 cxform_point(cfloat4x4 inv, cfloat3 p) { return cmul_point(inv, p); }

// Uniform scale (exact): evaluate at p/s, then multiply the distance by s.
CLAY_FN cfloat3 cscale_point(cfloat3 p, float s) { return p / s; }
CLAY_FN float cscale_dist(float d, float s) { return d * s; }

// Non-uniform scale (bound): evaluate at p/s, multiply by the SMALLEST
// factor — conservative, never overestimates.
CLAY_FN cfloat3 cscale_nu_point(cfloat3 p, cfloat3 s) { return p / s; }
CLAY_FN float cscale_nu_dist(float d, cfloat3 s) { return d * cmin(s.x, cmin(s.y, s.z)); }

// Elongation (exact for origin-symmetric primitives): evaluate the primitive
// at the returned point and ADD the returned correction (docs/01 §2.3).
CLAY_FN cfloat3 celongate_point(cfloat3 p, cfloat3 h, CLAY_THREAD float* correction) {
    cfloat3 q = cabs(p) - h;
    *correction = cmin(cmax(q.x, cmax(q.y, q.z)), 0.0f);
    return cmax(q, 0.0f);
}

// 1D-per-axis elongation (works for asymmetric primitives; flat interior
// plateau makes it a bound inside the stretch region).
CLAY_FN cfloat3 celongate_axis_point(cfloat3 p, cfloat3 h) {
    return p - cclamp(p, -h, h);
}

// Axis mirror folds (exact when the shape does not cross the mirror plane).
CLAY_FN cfloat3 csym_x(cfloat3 p) { return cf3(cabs(p.x), p.y, p.z); }
CLAY_FN cfloat3 csym_y(cfloat3 p) { return cf3(p.x, cabs(p.y), p.z); }
CLAY_FN cfloat3 csym_z(cfloat3 p) { return cf3(p.x, p.y, cabs(p.z)); }

// Fold across an arbitrary plane through the origin (n normalized): points on
// the negative side reflect to the positive side.
CLAY_FN cfloat3 cmirror_plane(cfloat3 p, cfloat3 n) {
    return p - 2.0f * cmin(cdot(p, n), 0.0f) * n;
}

// Reflect across the plane (for Mirror Blend the scene evaluates the item at
// both p and creflect(p, n) and smooth-unions the two results).
CLAY_FN cfloat3 creflect_plane(cfloat3 p, cfloat3 n) { return p - 2.0f * cdot(p, n) * n; }

// Rounding / dilation (exact): grows the surface outward by r (r < 0 erodes).
CLAY_FN float cop_round(float d, float r) { return d - r; }

// Onion / shell (exact): wall of thickness t centered on the surface.
CLAY_FN float cop_onion(float d, float t) { return cabs(d) - t; }

CLAY_NS_END
