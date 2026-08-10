#pragma once

// Repetition operators (docs/01 §2.4). round() maps to the NEAREST cell;
// finite repetition clamps the CELL INDEX, never the coordinate.
// Exactness condition: the repeated primitive plus its blend/round influence
// must fit its half-cell; otherwise the caller takes the min over neighbor
// cells (the scene compiler exposes this as padding).

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

// Infinite grid with cell size s (per axis).
CLAY_FN cfloat3 crep_inf_point(cfloat3 p, cfloat3 s) { return p - s * cround(p / s); }

// Finite grid: cell size s, extents l = max cell index per axis (grid spans
// [-l, +l] cells). Clamping the index keeps outermost copies un-smeared.
CLAY_FN cfloat3 crep_lim_point(cfloat3 p, float s, cfloat3 l) {
    return p - s * cclamp(cround(p / (s > 0.0f ? s : 1.0f)), -l, l);
}

// Neighbor-cell evaluation for padded (blend-carrying) repetitions: map p
// into the cell at integer offset `off` from its nearest cell.
CLAY_FN cfloat3 crep_inf_point_offset(cfloat3 p, cfloat3 s, cfloat3 off) {
    return p - s * (cround(p / s) + off);
}
CLAY_FN cfloat3 crep_lim_point_offset(cfloat3 p, float s, cfloat3 l, cfloat3 off) {
    return p - s * cclamp(cround(p / (s > 0.0f ? s : 1.0f)) + off, -l, l);
}

// Radial array of `count` copies around the Y axis. O(2) evaluation: map p
// into the nearest sector (offset 0) and its angular neighbor (offset ±1,
// picked by which side of the sector center p lies on), take the min of the
// two field evaluations.
CLAY_FN cfloat3 crep_radial_point(cfloat3 p, int count, int offset) {
    float sector = 6.2831853f / CLAY_FLOATC(count);
    float angle = catan2(p.z, p.x);
    float idx = cround(angle / sector) + CLAY_FLOATC(offset);
    float a = -idx * sector;  // rotate back into the canonical sector
    float c = ccos(a), s = csin(a);
    return cf3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
}

// Which neighbor to evaluate second (+1 or -1) for the O(2) scheme.
CLAY_FN int crep_radial_neighbor(cfloat3 p, int count) {
    float sector = 6.2831853f / CLAY_FLOATC(count);
    float angle = catan2(p.z, p.x);
    float frac = angle / sector - cround(angle / sector);
    return frac >= 0.0f ? 1 : -1;
}

CLAY_NS_END
