#pragma once

// A dense span of occupancy over a box of cells, shared by the two things that
// need to read a voxel grid as a CONTINUOUS field rather than as cells: the
// smooth mesher (#108) and the voxel-to-field conversion (#90).
//
// Materialised rather than read through cell_at, because both consumers read
// each cell many times — the mesher up to eight times per lattice point, a
// blur pass twenty-seven — and because a blur has to run over a dense buffer
// anyway.
//
// The cost is that this follows the occupied BOUNDING BOX rather than the
// material, which is the shape mesh_greedy's slab grouping exists to avoid.
// Bounded rather than solved: build() refuses a box no allocator could satisfy
// instead of asking for it.

#include <cstdint>
#include <vector>

#include "clay/voxel/grid.h"

namespace clay {
namespace voxel {
namespace detail {

struct OccupancyBox {
    int min[3]{0, 0, 0};
    int dim[3]{0, 0, 0};
    std::vector<float> value;

    std::size_t index(int x, int y, int z) const {
        return (static_cast<std::size_t>(z - min[2]) * static_cast<std::size_t>(dim[1]) +
                static_cast<std::size_t>(y - min[1])) *
                   static_cast<std::size_t>(dim[0]) +
               static_cast<std::size_t>(x - min[0]);
    }
    bool holds(int x, int y, int z) const {
        return x >= min[0] && x < min[0] + dim[0] && y >= min[1] && y < min[1] + dim[1] &&
               z >= min[2] && z < min[2] + dim[2];
    }
    float at(int x, int y, int z) const { return holds(x, y, z) ? value[index(x, y, z)] : 0.0f; }

    // Occupancy at a fractional cell coordinate, trilinear between cell
    // CENTRES. This is what makes the isosurface smooth rather than a
    // staircase without anything being filtered: the value between an occupied
    // and an empty cell varies continuously, so the 0.5 crossing lands
    // somewhere real instead of always on the cell boundary.
    float trilinear(float fx, float fy, float fz) const;
};

// Every occupied cell of `level`, or only those carrying `index` when it is
// non-zero, over the occupied extent padded by `pad` cells. Empty when the
// level holds nothing or the padded box would be unreasonable.
bool build_occupancy(const VoxelGrid& grid, std::size_t level, int pad, std::uint8_t index,
                     OccupancyBox* out);

// One pass of a separable 3x3x3 box filter. Extra smoothing a caller asked
// for, never a default: a single pass puts an isolated voxel near 0.3, under
// any sensible isolevel, and the voxel is gone.
void blur_occupancy(OccupancyBox& box);

}  // namespace detail
}  // namespace voxel
}  // namespace clay
