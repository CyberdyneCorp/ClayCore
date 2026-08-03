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

// Extrude-to / loft (bound): interpolate two profile distances along Z with
// an easing curve, then cap. d2a = bottom profile distance, d2b = top.
CLAY_FN float cop_extrude_to(float d2a, float d2b, float pz, float h, int ease_type) {
    float t = cease(ease_type, cclamp((pz + h) / (2.0f * h), 0.0f, 1.0f));
    float d2 = cmix(d2a, d2b, t);
    return cop_extrude(d2, pz, h);
}

CLAY_NS_END
