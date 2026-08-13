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
#include <functional>
#include <vector>

#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/surface_nets.h"
#include "clay/voxel/grid.h"

#include "occupancy_box.h"

namespace clay {
namespace voxel {

using kernel::cf3;
using kernel::cfloat3;


mesh::Mesh VoxelGrid::mesh_smooth(std::size_t level, SmoothOptions options) const {
    return dual_mesh(level, options.blur, /*cell_size=*/0.0f, /*keep_quads=*/false);
}

// The body of both. mesh_smooth is this with the grid's own cell size and no
// quads; mesh_quads' dual mode is this with the quads kept and, optionally, a
// coarser lattice. Keeping them ONE function is what makes "dual mode at the
// voxel size IS the smooth mesh" a property of the code rather than a
// coincidence two copies would eventually lose.
mesh::Mesh VoxelGrid::dual_mesh(std::size_t level, int blur_in, float cell_size,
                                bool keep_quads) const {
    mesh::Mesh out;
    if (level >= levels_.size()) return out;
    const ChunkMap& chunks = levels_[level].chunks;
    if (chunks.empty()) return out;

    // One cell of apron for the mesher to close the surface against, plus one
    // per blur pass so a sample near the edge reads the same zeros a sample
    // well outside would.
    const int blur = std::max(blur_in, 0);
    detail::OccupancyBox box;
    if (!detail::build_occupancy(*this, level, 2 + blur, 0, &box)) return out;

    for (int i = 0; i < blur; ++i) detail::blur_occupancy(box);

    // Negative inside, so the mesher's "wound toward positive field" puts the
    // normals outward, as it does for every SDF this library meshes.
    const float kIsolevel = 0.5f;
    const float vs = levels_[level].voxel_size;
    // A lattice FINER than the grid is clamped away: the occupancy is a step
    // field, so a finer lattice resamples the same steps and buys quads
    // without buying detail. Coarser is allowed and low-passes — it can drop a
    // one-voxel feature entirely, the same trade `blur` makes.
    const float cell = (std::isfinite(cell_size) && cell_size > vs) ? cell_size : vs;
    const bool native = cell == vs;

    // Lattice points ARE voxel centres, so the lattice sits half a cell off the
    // grid's own corners. Without this the smooth mesh would be offset from the
    // greedy one by half a voxel on every axis, which reads as the sculpt
    // shifting when a host toggles the display.
    const cfloat3 origin = cf3(0.5f * cell, 0.5f * cell, 0.5f * cell);

    int cell_min[3], cell_max[3];
    std::function<float(int, int, int)> sample;
    if (native) {
        // The lattice IS the cell grid, so occupancy is read directly. Byte
        // for byte what mesh_smooth has always done, and deliberately not
        // routed through the trilinear sampler below: at integer coordinates
        // that sampler is the same value only up to floating-point rounding,
        // and this path is asserted to be identical, not close.
        sample = [&box, kIsolevel](int x, int y, int z) { return kIsolevel - box.at(x, y, z); };
        for (int d = 0; d < 3; ++d) {
            cell_min[d] = box.min[d];
            cell_max[d] = box.min[d] + box.dim[d] - 1;
        }
    } else {
        // A lattice of its own: world point origin + i*cell, read from the
        // occupancy at the fractional CELL coordinate that point falls at
        // (integer coordinate = cell centre, hence the half).
        sample = [&box, kIsolevel, cell, vs](int x, int y, int z) {
            const float fx = (static_cast<float>(x) * cell + 0.5f * cell) / vs - 0.5f;
            const float fy = (static_cast<float>(y) * cell + 0.5f * cell) / vs - 0.5f;
            const float fz = (static_cast<float>(z) * cell + 0.5f * cell) / vs - 0.5f;
            return kIsolevel - box.trilinear(fx, fy, fz);
        };
        // Enough lattice points to span the occupancy box, apron included, so
        // the surface still closes against the zeros outside the material.
        for (int d = 0; d < 3; ++d) {
            const float lo = (static_cast<float>(box.min[d]) + 0.5f) * vs;
            const float hi = (static_cast<float>(box.min[d] + box.dim[d]) - 0.5f) * vs;
            cell_min[d] = static_cast<int>(std::floor(lo / cell - 0.5f));
            cell_max[d] = static_cast<int>(std::ceil(hi / cell - 0.5f));
        }
    }
    out = keep_quads ? mesh::mesh_lattice_quads(sample, cell_min, cell_max, origin, cell)
                     : mesh::mesh_lattice_nets(sample, cell_min, cell_max, origin, cell);

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
