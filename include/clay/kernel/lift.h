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

CLAY_NS_END
