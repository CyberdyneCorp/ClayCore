#pragma once

// Lifts of 2D profiles to 3D (docs/01 §2.6). Extrude and revolve are exact
// lifts of exact profiles; extrude-to (loft) lerps distance fields and is a
// bound.

#include "clay/kernel/shim.h"
#include "clay/kernel/ease.h"

CLAY_NS_BEGIN

// Exact extrusion along Z: d2 = profile distance at (p.x, p.y), half-depth h.
// The min/max+length pattern merges profile and slab distances exactly.
CLAY_FN float cop_extrude(float d2, float pz, float h) {
    cfloat2 w = cf2(d2, cabs(pz) - h);
    return cmin(cmax(w.x, w.y), 0.0f) + clength(cmax(w, 0.0f));
}

// Exact revolution around Y: evaluate the profile at the returned 2D point.
// o = offset of the profile from the axis.
CLAY_FN cfloat2 crevolve_point(cfloat3 p, float o) {
    return cf2(clength(cf2(p.x, p.z)) - o, p.y);
}

// Extrude-to / loft (BOUND): interpolate two profile distances and cap with
// the slab. `u` is the already-eased parameter between the two, which the
// caller computes — with more than two profiles it has to bracket first, and
// a signature that derived u from pz could only ever serve exactly two.
//
// Bound, not exact: a lerp of two distance fields is not a distance field.
// The interpolation also adds a Lipschitz term proportional to how far apart
// the two fields are over the depth they are mixed across, which the compiler
// declares — see cfi_loft.
CLAY_FN float cop_loft(float d2a, float d2b, float u, float pz, float h) {
    return cop_extrude(cmix(d2a, d2b, u), pz, h);
}

// One vertex of a swept guide as it sits in the blob: a position, a
// parallel-transported normal, and the arc length reached there.
#define CLAY_SWEPT_VERTEX_FLOATS 7

// Where a query point projects onto a guide polyline. Declared the way the
// tape's own structs are — typedef'd rather than a bare `struct` — because the
// OpenCL profile is C, where a bare tag needs the keyword at every use.
typedef struct CSweepHitT {
    int seg;      // index of the nearest segment
    float t;      // parameter along it, clamped to [0, 1]
    float dist2;  // squared distance to the guide
} CSweepHit;

// Nearest point on a guide polyline. Returns a struct rather than writing
// through out-parameters: a pointer needs an explicit address space in the
// Metal dialect, and a returned value needs none — the same reason CRayHit
// exists.
//
// A loop over segments, which is what ctape_stroke already does per sample —
// the same cost and the same pattern, so a sweep is not new ground for the
// evaluator even though it is new geometry.
//
// How much closer a segment must be to WIN, as a fraction of the incumbent's
// squared distance. Not a micro-optimisation — it is what makes the choice
// reproducible across backends.
//
// Any point whose nearest guide point is a shared VERTEX — the outside of
// every bend — is equidistant from the two segments meeting there, by
// construction. Those two carry different tangents, so they build different
// frames and resolve the profile at different arc lengths: which one wins can
// be worth several percent of the result, not an ulp. Yet the comparison that
// decides it is a knife edge, and backends disagree in the last ulp (an FMA
// contraction one compiler made and another did not is enough). Requiring a
// relative improvement keeps the earliest of a tied group everywhere.
//
// The margin has three orders of magnitude of room on both sides: cross-backend
// noise measured ~1e-7 relative, while the gap to the next genuinely different
// segment is ~7e-3 on a guide tessellated to a 0.03 curve tolerance.
#define CLAY_SWEEP_TIE_REL 1e-5f

CLAY_FN CSweepHit csweep_nearest(CLAY_FPTR guide, int count, cfloat3 p) {
    int best = 0;
    float best_t = 0.0f;
    float best_d2 = 3.4e38f;
    for (int i = 0; i + 1 < count; ++i) {
        CLAY_FPTR va = CLAY_OFF(guide, i * CLAY_SWEPT_VERTEX_FLOATS);
        CLAY_FPTR vb = CLAY_OFF(guide, (i + 1) * CLAY_SWEPT_VERTEX_FLOATS);
        cfloat3 a = cf3(CLAY_AT(va, 0), CLAY_AT(va, 1), CLAY_AT(va, 2));
        cfloat3 b = cf3(CLAY_AT(vb, 0), CLAY_AT(vb, 1), CLAY_AT(vb, 2));
        cfloat3 ab = b - a;
        float len2 = cdot(ab, ab);
        float t = len2 > 1e-12f ? cclamp(cdot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        cfloat3 offset = p - (a + ab * t);
        float d2 = cdot(offset, offset);
        if (d2 < best_d2 - CLAY_SWEEP_TIE_REL * best_d2) {
            best_d2 = d2;
            best = i;
            best_t = t;
        }
    }
    CSweepHit hit;
    hit.seg = best;
    hit.t = best_t;
    hit.dist2 = best_d2;
    return hit;
}

// The moving frame at a hit: where on the guide, which way it points, and how
// far along it that is.
//
// Shared rather than written once per caller. A sweep asks "given an arc
// length, where does the profile sit"; bending along a curve asks the same
// question backwards — "given a point, what arc length is it at, and where in
// that frame". They are the same geometry read from either end, and sharing
// the construction is what makes them agree by construction rather than by
// inspection.
typedef struct CSweepFrameT {
    cfloat3 origin;    // the point on the guide the hit projected to
    cfloat3 tangent;
    cfloat3 normal;    // parallel-transported, so it neither flips nor vanishes
    cfloat3 binormal;
    float arclen;      // arc length reached at `origin`
    float seg_len;
} CSweepFrame;

CLAY_FN CSweepFrame csweep_frame(CLAY_FPTR guide, CSweepHit hit) {
    CLAY_FPTR va = CLAY_OFF(guide, hit.seg * CLAY_SWEPT_VERTEX_FLOATS);
    CLAY_FPTR vb = CLAY_OFF(va, CLAY_SWEPT_VERTEX_FLOATS);
    cfloat3 a = cf3(CLAY_AT(va, 0), CLAY_AT(va, 1), CLAY_AT(va, 2));
    cfloat3 b = cf3(CLAY_AT(vb, 0), CLAY_AT(vb, 1), CLAY_AT(vb, 2));
    cfloat3 ab = b - a;
    float seg_len = clength(ab);

    CSweepFrame f;
    f.seg_len = seg_len;
    f.origin = a + ab * hit.t;
    f.tangent = seg_len > 1e-9f ? ab * (1.0f / seg_len) : cf3(0, 0, 1);

    // The two end normals were transported when the item compiled; lerp and
    // re-orthogonalize, which is enough for a polyline guide and avoids a
    // slerp in the inner loop.
    cfloat3 na = cf3(CLAY_AT(va, 3), CLAY_AT(va, 4), CLAY_AT(va, 5));
    cfloat3 nb = cf3(CLAY_AT(vb, 3), CLAY_AT(vb, 4), CLAY_AT(vb, 5));
    cfloat3 n = cnormalize(cmix(na, nb, hit.t));
    f.normal = cnormalize(n - f.tangent * cdot(n, f.tangent));
    f.binormal = ccross(f.tangent, f.normal);
    f.arclen = CLAY_AT(va, 6) + hit.t * seg_len;
    return f;
}

// Total arc length of a guide: the last vertex's own reading.
CLAY_FN float csweep_length(CLAY_FPTR guide, int count) {
    return count < 2 ? 0.0f : CLAY_AT(guide, (count - 1) * CLAY_SWEPT_VERTEX_FLOATS + 6);
}

CLAY_NS_END
