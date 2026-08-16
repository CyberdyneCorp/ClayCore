#pragma once

// Deformers — metric breakers, all bound fields (docs/01 §2.5).
// Point-map deformers return the warped point at which to evaluate the
// wrapped field; weight helpers return spatial morph factors for the
// transition ops (the interpreter mixes the two subtree results).
// Every deformer's Lipschitz factor is provided by clay/kernel/exactness.h
// so consumers scale their steps; kernels here only warp.

#include "clay/kernel/noise.h"
#include "clay/kernel/shim.h"
#include "clay/kernel/ease.h"
// For the guide query and moving frame, which bending along a curve shares
// with the swept primitive rather than reimplementing.
#include "clay/kernel/lift.h"

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

// RANGED twist and bend — ZBrush's Gizmo 3D acts inside the gizmo's box, and
// the two above act on the whole item. The ranged pair ramps the rotation
// across [t0, t1] with an easing curve and HOLDS it beyond, so the material
// past the range travels rigidly instead of continuing to wind.
//
// Deliberately the same rotation as the unranged form with the angle
// substituted, rather than a second formulation: with a linear ease and a
// range that covers the content, `ctwist_range_point(p, k, y0, y1, 0)` is
// `ctwist_point(p, k)` for every point inside the range, which is asserted
// rather than assumed. That is what makes this a range on an existing
// deformation rather than a new one to keep in step.
CLAY_FN cfloat3 ctwist_range_point(cfloat3 p, float k, float y0, float y1, int ease_type) {
    float t = cease(ease_type, cclamp((p.y - y0) / (y1 - y0), 0.0f, 1.0f));
    float angle = k * (y1 - y0) * t;
    float c = ccos(angle), s = csin(angle);
    return cf3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
}

CLAY_FN cfloat3 cbend_range_point(cfloat3 p, float k, float x0, float x1, int ease_type) {
    float t = cease(ease_type, cclamp((p.x - x0) / (x1 - x0), 0.0f, 1.0f));
    float angle = k * (x1 - x0) * t;
    float c = ccos(angle), s = csin(angle);
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

// Magnify and pinch: a RADIAL SCALE about a centre, with finite support.
//
// One deformation, one signed strength. Maxon's own page has it — "Magnify:
// pushes vertices away from cursor; inverse of Pinch" — so giving the two
// directions separate opcodes would be building the same thing twice with the
// sign flipped.
//
// Inverse map, like grab: to make the surface read as pushed AWAY from the
// centre, sample NEARER it. `strength` is the fraction of the offset removed at
// the centre of the region, so 0.5 samples at half the radius there and the
// shape reads as twice the size; a negative one pushes the sample outward and
// the shape gathers.
//
// The scale is clamped away from zero. At a factor of zero every point in the
// region would sample the centre, collapsing the neighbourhood to one value —
// a flat blob whose surface is wherever that value's isosurface happens to
// fall, which is not a magnification of anything.
CLAY_FN cfloat3 cmagnify_point(cfloat3 p, cfloat3 centre, float radius, float strength,
                               int ease_type) {
    cfloat3 v = p - centre;
    float w = cregion_weight(p, centre, radius, ease_type);
    if (w <= 0.0f) return p;  // finite support: untouched outside the radius
    float scale = cmax(1.0f - strength * w, 0.05f);
    return centre + v * scale;
}

// pose along a line: the weight ramps from the anchor to the end along the
// projection onto the segment, and the rotation is about the axis THROUGH the
// anchor, so the anchor is a fixed point.
//
// Unlike grab and radial pose this has NO finite support: the weight clamps, so
// material past the end is fully rotated rather than untouched. That is the
// tool — the whole limb tip moves — but it means the bound has to cover a swept
// arc rather than a dilation.
//
// As with grab, the weight is taken at the SAMPLE point rather than at its
// preimage, so the projection is measured in deformed space. The result is a
// bend rather than a rigid swing: the form curves away from the anchor and the
// achieved rotation falls short of the nominal angle as it grows — at a quarter
// turn a straight capsule reads as a hockey stick, not an L. That is the
// behaviour a tapered pose brush wants, and recovering the exact swing would
// cost an iteration per sample.
CLAY_FN cfloat3 cpose_line_point(cfloat3 p, cfloat3 a, cfloat3 b, cfloat3 axis, float angle,
                                 int ease_type) {
    cfloat3 ab = b - a;
    float len2 = cdot(ab, ab);
    float axis_len = clength(axis);
    if (len2 < 1e-12f || axis_len < 1e-9f) return p;
    cfloat3 unit = axis * (1.0f / axis_len);
    float w = cease(ease_type, cclamp(cdot(p - a, ab) / len2, 0.0f, 1.0f));
    float theta = -angle * w;  // inverse map
    cfloat3 v = p - a;
    float s = csin(theta), c = ccos(theta);
    cfloat3 rotated = v * c + ccross(unit, v) * s + unit * (cdot(unit, v) * (1.0f - c));
    return a + rotated;
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

// Bend along a DRAWN guide rather than at a constant rate — ZBrush's Gizmo 3D
// Bend Curve. `bend` and `bend_range` turn about a fixed axis, so every bend
// they can express is a circular arc; this lays the item's local X span
// [t0, t1] onto the guide's ARC LENGTH and carries the material on the guide's
// parallel-transported frames.
//
// The INVERSE of Prim::swept, and deliberately built from the sweep's own
// query. A sweep asks "given an arc length, where does the profile sit"; a
// deformer is an inverse point map and asks the opposite — "given a point,
// what arc length is it at, and where in that frame". Same geometry, read from
// the other end.
//
// A guide running straight along X is the IDENTITY: the transported normal is
// constant, s = p.x - t0, and the map returns p unchanged. That is what makes
// this a generalization of the undeformed item rather than a second thing to
// keep in step with it.
CLAY_FN cfloat3 cbend_curve_point(CLAY_FPTR guide, int count, cfloat3 p, float t0, float t1) {
    if (count < 2) return p;
    CSweepHit hit = csweep_nearest(guide, count, p);
    CSweepFrame f = csweep_frame(guide, hit);
    float total = csweep_length(guide, count);

    cfloat3 offset = p - f.origin;
    float axial = cdot(offset, f.tangent);
    cfloat3 perp = offset - f.tangent * axial;

    // Everywhere but the ends the projection makes the offset perpendicular by
    // construction, so `axial` is zero and adding it changes nothing. Past an
    // end the nearest point was CLAMPED, and the leftover tangential part is
    // the overshoot — it has to keep travelling, or every point beyond the
    // guide would collapse onto its end cap.
    float s = f.arclen;
    if (hit.seg == 0 && hit.t <= 0.0f) s += axial;
    if (hit.seg == count - 2 && hit.t >= 1.0f) s += axial;

    float u = total > 1e-9f ? s / total : 0.0f;
    return cf3(t0 + u * (t1 - t0), cdot(perp, f.normal), cdot(perp, f.binormal));
}

// A LATTICE (free-form deformation) cage — ZBrush's Gizmo Lattice, on an SDF
// item rather than on vertices.
//
// The cage's control-point offsets ARE THE INVERSE WARP, which is the whole
// design decision. Forward FFD has no closed-form inverse, and a claycore
// deformer must run backwards: it answers "where did the material at p come
// from". The three ways out are Newton-inverting per sample (iteration inside
// every backend's inner loop — not this dialect), baking through a sampled
// volume (exact, and the cage stops being editable), or authoring the cage AS
// the inverse. This is the third.
//
// What that costs, stated rather than implied: this is NOT the exact inverse
// of forward FFD. The two differ by a term proportional to how the basis VARIES
// along the displacement, so the error points the way the basis gradient does —
// it over-travels a drag toward rising weight and under-travels one pointing
// away. `grab`'s weight always falls off along its drag, which is why that one
// always under-travels; a lattice does not inherit the sign. Measured against
// the forward cage on a mesh layer, the difference is under 1.5% of the drag
// (examples/50_sdf_lattice.py).
//
// Cost per sample is nx * ny * nz multiply-adds, which is why divisions are
// capped low here and not on the mesh lattice: that one evaluates once per
// vertex, this one runs inside the raymarcher.
#define CLAY_LATTICE_MAX_DIV 4

// Where p sits in the box on one axis, clamped to [0, 1].
//
// Clamped because the offset field is only DEFINED over the cage: a point
// beyond it takes the offset of the nearest part and travels rigidly. A
// degenerate axis reads as the middle, so none of its control points are dead.
CLAY_FN float clattice_param(float v, float lo, float hi) {
    float span = hi - lo;
    if (!(span > 1e-9f)) return 0.5f;
    return cclamp((v - lo) / span, 0.0f, 1.0f);
}

CLAY_FN cfloat3 clattice_point(CLAY_FPTR offsets, int nx, int ny, int nz, cfloat3 lo, cfloat3 hi,
                               cfloat3 p) {
    if (nx < 2 || ny < 2 || nz < 2) return p;
    // ONE FLAT local array, and the recurrence written inline rather than in a
    // helper. An array out-parameter is not portable across this dialect —
    // Metal wants an address space on the pointer and GLSL wants a sized inout
    // — and a flat local is the spelling every profile accepts.
    float basis[3 * CLAY_LATTICE_MAX_DIV];
    int n[3];
    float t[3];
    n[0] = nx;
    n[1] = ny;
    n[2] = nz;
    t[0] = clattice_param(p.x, lo.x, hi.x);
    t[1] = clattice_param(p.y, lo.y, hi.y);
    t[2] = clattice_param(p.z, lo.z, hi.z);
    for (int a = 0; a < 3; ++a) {
        // De Casteljau's recurrence rather than binomials times powers: no
        // factorials, nothing to overflow, better conditioned.
        int base = a * CLAY_LATTICE_MAX_DIV;
        basis[base] = 1.0f;
        for (int d = 1; d < n[a]; ++d) {
            basis[base + d] = t[a] * basis[base + d - 1];
            for (int i = d - 1; i > 0; --i)
                basis[base + i] = (1.0f - t[a]) * basis[base + i] + t[a] * basis[base + i - 1];
            basis[base] = (1.0f - t[a]) * basis[base];
        }
    }

    cfloat3 sum = cf3(0, 0, 0);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                float w = basis[i] * basis[CLAY_LATTICE_MAX_DIV + j] *
                          basis[2 * CLAY_LATTICE_MAX_DIV + k];
                int at = ((k * ny + j) * nx + i) * 3;
                sum = sum + cf3(CLAY_AT(offsets, at), CLAY_AT(offsets, at + 1),
                                CLAY_AT(offsets, at + 2)) * w;
            }
    // MINUS, and this is the whole inverse-cage convention in one operator.
    //
    // The offsets are what the artist DRAGGED, so that dragging a control
    // point +X moves material +X — which is what a lattice means, and what the
    // mesh lattice does with a plus for the same reason. This one is the
    // INVERSE map, so it samples the undeformed field on the opposite side.
    //
    // Storing the already-negated sample offset instead would make the two
    // lattices disagree about what a positive offset means, and every caller
    // would have to remember which was which.
    return p - sum;
}

// The same cage, applied through a TRANSFORM: map the point into the cage's own
// space, warp it there, map it back.
//
//     p' = Tinv( T(p) + D(T(p)) )
//
// This is what lets ONE cage placed in the world act on items in any frame. A
// lattice box is axis-aligned by construction, so a world-axis-aligned cage is
// not axis-aligned in a rotated item's local space and no per-item box
// reproduces it. Resampling the cage onto a per-item grid would approximate
// what this does exactly.
//
// A SEPARATE ENTRY POINT rather than a flag on clattice_point, so the
// axis-aligned cage pays nothing for this existing — adding per-sample work to
// a path that does not need it is the defect #137 and #140 had to undo.
//
// `blob` holds the forward transform (12 floats, column-major affine), then its
// inverse (12), then the offsets. The inverse is stored rather than derived
// because deriving it per sample is a quaternion conjugate and a divide, every
// sample, to recover something known at compile time.
CLAY_FN cfloat3 caffine12(CLAY_FPTR m, cfloat3 p) {
    return cf3(CLAY_AT(m, 0) * p.x + CLAY_AT(m, 3) * p.y + CLAY_AT(m, 6) * p.z + CLAY_AT(m, 9),
               CLAY_AT(m, 1) * p.x + CLAY_AT(m, 4) * p.y + CLAY_AT(m, 7) * p.z + CLAY_AT(m, 10),
               CLAY_AT(m, 2) * p.x + CLAY_AT(m, 5) * p.y + CLAY_AT(m, 8) * p.z + CLAY_AT(m, 11));
}

CLAY_FN cfloat3 clattice_xform_point(CLAY_FPTR blob, int nx, int ny, int nz, cfloat3 lo, cfloat3 hi,
                                     cfloat3 p) {
    cfloat3 q = caffine12(blob, p);
    cfloat3 warped = clattice_point(CLAY_OFF(blob, 24), nx, ny, nz, lo, hi, q);
    return caffine12(CLAY_OFF(blob, 12), warped);
}

// BLOB — ZBrush's Blob: an irregular swelling under the brush, rather than the
// smooth one `draw` gives.
//
// It is `noise` with FINITE SUPPORT, and deliberately nothing more. The
// fractal is the same `cnoise_fbm` the whole-item noise deformer offsets the
// distance by; what this adds is `cregion_weight`, the same radial falloff
// `grab`, `pose` and `magnify` use. Outside the radius the weight is zero, so
// the field is untouched — which is what makes it a brush rather than a
// modifier, and what lets the influence bound stay tight.
//
// A DISTANCE OFFSET, not a point warp, for the same reason noise is one: the
// irregularity wanted here is the surface moving in and out along its own
// normal, and offsetting the distance does exactly that. Warping the point
// would slide material sideways instead.
//
// The sign is the artist's: a positive amplitude swells, a negative one eats
// in, and the noise is signed so both happen within one dab — which is what
// makes it read as blobby rather than as a uniform bulge.
CLAY_FN float cblob_offset(cfloat3 p, cfloat3 centre, float radius, float amplitude,
                           float frequency, int octaves, float gain, unsigned int seed,
                           int ease_type) {
    float w = cregion_weight(p, centre, radius, ease_type);
    if (w <= 0.0f) return 0.0f;
    return amplitude * w * cnoise_fbm(p * frequency, octaves, gain, seed);
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
