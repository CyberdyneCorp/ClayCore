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
CLAY_FN CSweepHit csweep_nearest(CLAY_DEVICE const float* guide, int count, cfloat3 p) {
    int best = 0;
    float best_t = 0.0f;
    float best_d2 = 3.4e38f;
    for (int i = 0; i + 1 < count; ++i) {
        CLAY_DEVICE const float* va = guide + i * CLAY_SWEPT_VERTEX_FLOATS;
        CLAY_DEVICE const float* vb = guide + (i + 1) * CLAY_SWEPT_VERTEX_FLOATS;
        cfloat3 a = cf3(va[0], va[1], va[2]);
        cfloat3 b = cf3(vb[0], vb[1], vb[2]);
        cfloat3 ab = b - a;
        float len2 = cdot(ab, ab);
        float t = len2 > 1e-12f ? cclamp(cdot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        cfloat3 offset = p - (a + ab * t);
        float d2 = cdot(offset, offset);
        if (d2 < best_d2) {
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

CLAY_NS_END
