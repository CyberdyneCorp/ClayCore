#pragma once

// The cell-coordinate dither, shared by the falloff brushes and by sculpt
// layers.
//
// ONE definition on purpose. It is what makes a soft stroke land on the same
// cells on every platform and backend — a property the parity suite enforces —
// and sculpt layers reuse it rather than inventing a second: a layer at 40%
// keeps a reproducible 40% of its cells by the same rule a brush at 40%
// strength touches 40% of its footprint. A reader who has met one has met both.
//
// Internal to the voxel module; nothing outside it needs to know how a
// fractional strength picks cells, only that it is reproducible.

#include <cstdint>

#include "clay/voxel/grid.h"

namespace clay {
namespace voxel {
namespace dither {

// Deterministic per-cell threshold in [0, 1). Same cell + seed => same value
// on every platform: integer mixing only, no floating point until the end.
inline float cell_threshold(VoxelCoord c, std::uint32_t seed) {
    std::uint32_t h = seed * 0x9E3779B9u;
    h ^= static_cast<std::uint32_t>(c.x) * 0x85EBCA6Bu;
    h = (h ^ (h >> 13)) * 0xC2B2AE35u;
    h ^= static_cast<std::uint32_t>(c.y) * 0x27D4EB2Fu;
    h = (h ^ (h >> 15)) * 0x165667B1u;
    h ^= static_cast<std::uint32_t>(c.z) * 0x9E3779B1u;
    h = (h ^ (h >> 16)) * 0x7FEB352Du;
    h ^= h >> 15;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

// Whether this cell is touched at the given weight. Weight >= 1 always passes
// and <= 0 never does, so full strength is exactly the undithered result and
// zero strength is exactly nothing — both EXACT, which is what lets a layer at
// 1.0 equal applying the pass directly and a layer at 0.0 equal not having it.
inline bool passes(VoxelCoord world, float weight, std::uint32_t seed) {
    if (weight >= 1.0f) return true;
    if (weight <= 0.0f) return false;
    return weight > cell_threshold(world, seed);
}

}  // namespace dither
}  // namespace voxel
}  // namespace clay
