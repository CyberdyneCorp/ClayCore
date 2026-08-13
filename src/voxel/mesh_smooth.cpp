// Displaying a voxel sculpt as a form rather than as boxes (#108).
//
// mesh_greedy emits axis-aligned quads, which is what greedy meshing IS and is
// correct for hard-surface work and for export. It is the wrong picture of an
// organic sculpt, and it was the whole reason every voxel render in the
// gallery looked like Minecraft while every SDF render looked like clay.
//
// The mesher here is mesh::mesh_lattice_nets, unchanged — it already takes a
// SAMPLER rather than a tape, so the only thing missing was something to
// sample. What follows is that sampler.

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/mesh/surface_nets.h"
#include "clay/voxel/grid.h"

#include "occupancy_box.h"

namespace clay {
namespace voxel {

using kernel::cf3;
using kernel::cfloat3;


mesh::Mesh VoxelGrid::mesh_smooth(std::size_t level, SmoothOptions options) const {
    mesh::Mesh out;
    if (level >= levels_.size()) return out;
    const ChunkMap& chunks = levels_[level].chunks;
    if (chunks.empty()) return out;

    // One cell of apron for the mesher to close the surface against, plus one
    // per blur pass so a sample near the edge reads the same zeros a sample
    // well outside would.
    const int blur = std::max(options.blur, 0);
    detail::OccupancyBox box;
    if (!detail::build_occupancy(*this, level, 2 + blur, 0, &box)) return out;

    for (int i = 0; i < blur; ++i) detail::blur_occupancy(box);

    // Negative inside, so the mesher's "wound toward positive field" puts the
    // normals outward, as it does for every SDF this library meshes.
    const float kIsolevel = 0.5f;
    auto sample = [&](int x, int y, int z) { return kIsolevel - box.at(x, y, z); };

    const int cell_min[3] = {box.min[0], box.min[1], box.min[2]};
    const int cell_max[3] = {box.min[0] + box.dim[0] - 1, box.min[1] + box.dim[1] - 1,
                             box.min[2] + box.dim[2] - 1};
    const float vs = levels_[level].voxel_size;
    // Lattice points ARE voxel centres, so the lattice sits half a cell off the
    // grid's own corners. Without this the smooth mesh would be offset from the
    // greedy one by half a voxel on every axis, which reads as the sculpt
    // shifting when a host toggles the display.
    const cfloat3 origin = cf3(0.5f * vs, 0.5f * vs, 0.5f * vs);
    out = mesh::mesh_lattice_nets(sample, cell_min, cell_max, origin, vs);

    // Colour per vertex, from the palette entries actually around it.
    //
    // A smooth surface has no facet to carry one palette index the way a greedy
    // quad does: a vertex sits between up to eight voxels. Averaging the
    // OCCUPIED ones among them is what makes two colours meet in a gradient
    // across a cell instead of a hard line the geometry no longer has. Empty
    // neighbours are skipped rather than counted as black, which would darken
    // every silhouette.
    out.colors.resize(out.positions.size());
    for (std::size_t i = 0; i < out.positions.size(); ++i) {
        const cfloat3 p = out.positions[i];
        // Lattice point (i,j,k) IS voxel (i,j,k)'s centre, so a vertex inside
        // the lattice cell between points i and i+1 sits between voxels i and
        // i+1. Subtracting the half cell before flooring is what makes that
        // the pair looked up; flooring the raw position lands one cell late on
        // the far half of the span and reads two cells that can both be empty.
        const int cx = static_cast<int>(std::floor(p.x / vs - 0.5f));
        const int cy = static_cast<int>(std::floor(p.y / vs - 0.5f));
        const int cz = static_cast<int>(std::floor(p.z / vs - 0.5f));
        cfloat3 sum = cf3(0, 0, 0);
        int n = 0;
        for (int dz = 0; dz <= 1; ++dz)
            for (int dy = 0; dy <= 1; ++dy)
                for (int dx = 0; dx <= 1; ++dx) {
                    const std::uint8_t idx = cell_at(level, {cx + dx, cy + dy, cz + dz});
                    if (idx == 0) continue;
                    sum = sum + palette_color(idx);
                    ++n;
                }
        out.colors[i] = n > 0 ? sum * (1.0f / static_cast<float>(n)) : cf3(0.7f, 0.7f, 0.7f);
    }
    return out;
}

}  // namespace voxel
}  // namespace clay
