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
// Extended blend vocabulary (docs/01 §2.2, hg_sdf-style algebraic variants):
// op_groove/op_tongue/op_pipe/op_engrave/op_emboss/op_inset/op_shell_union/
// op_replace at the bottom of this header. All are rigid in the band-clamped
// sense: wherever the second operand's field b exceeds the op's support width
// S plus the band, clamp(result, ±band) equals clamp(a, ±band). Per-mode S
// lives in ccombine_extended_support (tape.h).

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

// -- extended blend vocabulary (a = accumulated field, b = item field) -------
// Sign convention below: "carves" raises the field toward positive (removes
// material from a), "adds" lowers it. Support widths in ops.h header comment.

// Solid pipe of radius r along the intersection curve of the two surfaces
// (adds material where both |a| and |b| are small). Support: r.
CLAY_FN float op_pipe(float a, float b, float r) {
    return cmin(a, clength(cf2(a, b)) - r);
}

// Engrave (deboss): carve a 45-degree V-groove of depth r into a's surface
// where b's surface crosses it. Keeps a elsewhere. Support: r.
// The diagonal term has Lipschitz up to sqrt(2) (see cfi_extended_blend).
CLAY_FN float op_engrave(float a, float b, float r) {
    return cmax(a, (a + r - cabs(b)) * 0.70710678f);
}

// Emboss: the dual of engrave — raise a 45-degree V-ridge of height r on a's
// surface along b's surface. Adds material (result <= a) only near b's
// surface; equals a elsewhere. Support: r.
CLAY_FN float op_emboss(float a, float b, float r) {
    return cmin(a, (a - r + cabs(b)) * 0.70710678f);
}

// Groove: carve a flat-bottomed channel of depth ra and half-width rb along
// b's surface. Support: rb (ra only caps the depth).
CLAY_FN float op_groove(float a, float b, float ra, float rb) {
    return cmax(a, cmin(a + ra, rb - cabs(b)));
}

// Tongue: raise a flat-topped ridge of height ra and half-width rb along b's
// surface. Support: rb.
CLAY_FN float op_tongue(float a, float b, float ra, float rb) {
    return cmin(a, cmax(a - ra, cabs(b) - rb));
}

// Inset: recessed panel — subtract b from a with the carve depth clamped to
// r (the panel floor is a's surface eroded by r, walls follow b). Note the
// -b: the recess covers b's interior. Support: 0 (deviation is confined to
// where -b > a, inside b's bound).
CLAY_FN float op_inset(float a, float b, float r) {
    return cmax(a, cmin(a + r, -b));
}

// Shell: union a with the shell of b (wall half-thickness t, centered on
// b's surface — cop_onion applied to the operand). Support: t.
CLAY_FN float op_shell_union(float a, float b, float t) {
    return cmin(a, cabs(b) - t);
}

// Replace: (a minus b) union b — b's field replaces a inside b. Support: 0
// (both the carve max(a, -b) and the union min(., b) are decided by b's own
// sign outside its bound).
CLAY_FN float op_replace(float a, float b) {
    return cmin(cmax(a, -b), b);
}

CLAY_NS_END
