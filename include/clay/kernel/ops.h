#pragma once

// CSG booleans and blend operators (docs/01 §2.1–2.2).
//
// Every smooth blend here is RIGID (finite support): outside its support
// width the result is bit-identical to the hard boolean. Support widths are
// exposed as c*_support() so the scene module can build influence bounds
// from them (scene-model spec: influence bounds = AABB ⊕ blend ⊕ rounding).
// Chamfer additionally deviates in the deep interior (both operands very
// negative), which never moves the surface or the narrow band.
//
// Material mixing: the *_m variants return cf2(distance, mix) where mix is
// the color interpolation weight toward the second operand (docs/01 §2.2).
//
// Extended blend vocabulary (groove, tongue, emboss, ...) lands with the
// Phase 2 task 11.5.

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

// -- hard booleans (exact, with the interior-seam caveat of docs/01 §2.1) ----

CLAY_FN float op_union(float d1, float d2) { return cmin(d1, d2); }
CLAY_FN float op_subtract(float d1, float d2) { return cmax(-d1, d2); }  // d2 minus d1
CLAY_FN float op_intersect(float d1, float d2) { return cmax(d1, d2); }
CLAY_FN float op_xor(float d1, float d2) { return cmax(cmin(d1, d2), -cmax(d1, d2)); }

// -- smooth minimum family (k = blend parameter, world units) ----------------

CLAY_FN float csmin_quadratic_support(float k) { return 4.0f * k; }
CLAY_FN float csmin_cubic_support(float k) { return 6.0f * k; }
CLAY_FN float csmin_circular_support(float k) { return k / (1.0f - 0.70710678f); }
CLAY_FN float cchamfer_support(float k) { return k; }

// Quadratic polynomial — C1, the industry-standard blend.
CLAY_FN float csmin_quadratic(float a, float b, float k) {
    float s = csmin_quadratic_support(k);
    if (s <= 0.0f) return cmin(a, b);
    float h = cmax(s - cabs(a - b), 0.0f) / s;
    return cmin(a, b) - h * h * s * 0.25f;
}

// Cubic — C2 (curvature-continuous; no shading kinks in reflections).
CLAY_FN float csmin_cubic(float a, float b, float k) {
    float s = csmin_cubic_support(k);
    if (s <= 0.0f) return cmin(a, b);
    float h = cmax(s - cabs(a - b), 0.0f) / s;
    return cmin(a, b) - h * h * h * s * (1.0f / 6.0f);
}

// Circular — the blend cross-section is an exact circular fillet.
CLAY_FN float csmin_circular(float a, float b, float k) {
    float s = csmin_circular_support(k);
    if (s <= 0.0f) return cmin(a, b);
    float h = cmax(s - cabs(a - b), 0.0f) / s;
    return cmin(a, b) - s * 0.5f * (1.0f + h - csqrt(1.0f - h * (h - 2.0f)));
}

// Chamfer — linear profile, a 45° flat instead of a fillet.
CLAY_FN float cchamfer(float a, float b, float k) {
    return cmin(cmin(a, b), (a + b - k) * 0.70710678f);
}

// -- smooth booleans via De Morgan (per profile) -----------------------------

CLAY_FN float op_sunion_quadratic(float d1, float d2, float k) { return csmin_quadratic(d1, d2, k); }
CLAY_FN float op_ssubtract_quadratic(float d1, float d2, float k) {
    return -csmin_quadratic(-d2, d1, k);
}
CLAY_FN float op_sintersect_quadratic(float d1, float d2, float k) {
    return -csmin_quadratic(-d1, -d2, k);
}

CLAY_FN float op_sunion_cubic(float d1, float d2, float k) { return csmin_cubic(d1, d2, k); }
CLAY_FN float op_ssubtract_cubic(float d1, float d2, float k) { return -csmin_cubic(-d2, d1, k); }
CLAY_FN float op_sintersect_cubic(float d1, float d2, float k) { return -csmin_cubic(-d1, -d2, k); }

CLAY_FN float op_sunion_circular(float d1, float d2, float k) { return csmin_circular(d1, d2, k); }
CLAY_FN float op_ssubtract_circular(float d1, float d2, float k) {
    return -csmin_circular(-d2, d1, k);
}
CLAY_FN float op_sintersect_circular(float d1, float d2, float k) {
    return -csmin_circular(-d1, -d2, k);
}

CLAY_FN float op_chamfer_union(float d1, float d2, float k) { return cchamfer(d1, d2, k); }
CLAY_FN float op_chamfer_subtract(float d1, float d2, float k) { return -cchamfer(-d2, d1, k); }
CLAY_FN float op_chamfer_intersect(float d1, float d2, float k) { return -cchamfer(-d1, -d2, k); }

// -- material-mix variants: cf2(distance, mix toward operand b) --------------

CLAY_FN cfloat2 csmin_quadratic_m(float a, float b, float k) {
    float s = cmax(csmin_quadratic_support(k), 1e-20f);
    float h = 1.0f - cmin(cabs(a - b) / s, 1.0f);
    float w = h * h;
    float off = w * s * 0.25f;
    return (a < b) ? cf2(a - off, w * 0.5f) : cf2(b - off, 1.0f - w * 0.5f);
}

CLAY_FN cfloat2 csmin_cubic_m(float a, float b, float k) {
    float s = cmax(csmin_cubic_support(k), 1e-20f);
    float h = 1.0f - cmin(cabs(a - b) / s, 1.0f);
    float w = h * h * h;
    float off = w * s * (1.0f / 6.0f);
    return (a < b) ? cf2(a - off, w * 0.5f) : cf2(b - off, 1.0f - w * 0.5f);
}

CLAY_FN cfloat2 csmin_circular_m(float a, float b, float k) {
    float s = cmax(csmin_circular_support(k), 1e-20f);
    float h = 1.0f - cmin(cabs(a - b) / s, 1.0f);
    float d = cmin(a, b) - s * 0.5f * (1.0f + h - csqrt(1.0f - h * (h - 2.0f)));
    return (a < b) ? cf2(d, h * 0.5f) : cf2(d, 1.0f - h * 0.5f);
}

CLAY_FN cfloat2 cchamfer_m(float a, float b, float k) {
    float h = 1.0f - cmin(cabs(a - b) / cmax(k, 1e-20f), 1.0f);
    float d = cchamfer(a, b, k);
    return (a < b) ? cf2(d, h * 0.5f) : cf2(d, 1.0f - h * 0.5f);
}

CLAY_NS_END
