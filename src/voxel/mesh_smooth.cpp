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

namespace clay {
namespace voxel {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// Occupancy over a box of cells, as floats in [0,1], with a one-cell apron on
// every side so the mesher can read a neighbour outside the sculpt and close
// the surface against it.
//
// Materialised rather than sampled through cell_at: mesh_lattice_nets reads
// every lattice point up to eight times (once per adjacent cell), and a blur
// pass reads twenty-seven. A dense span of floats over the occupied box is
// cheaper than either and is what a blur has to run over anyway.
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
    float at(int x, int y, int z) const {
        return holds(x, y, z) ? value[index(x, y, z)] : 0.0f;
    }
};

// One pass of a separable 3x3x3 box filter. Extra smoothing a caller asked
// for, never the default: a single pass puts an isolated voxel near 0.3, which
// is under the isolevel, and the voxel disappears.
void blur_once(OccupancyBox& box) {
    std::vector<float> scratch(box.value.size());
    const int x0 = box.min[0], y0 = box.min[1], z0 = box.min[2];
    const int x1 = x0 + box.dim[0], y1 = y0 + box.dim[1], z1 = z0 + box.dim[2];
    auto pass = [&](int dx, int dy, int dz) {
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    scratch[box.index(x, y, z)] =
                        (box.at(x - dx, y - dy, z - dz) + box.at(x, y, z) +
                         box.at(x + dx, y + dy, z + dz)) /
                        3.0f;
        box.value.swap(scratch);
    };
    pass(1, 0, 0);
    pass(0, 1, 0);
    pass(0, 0, 1);
}

}  // namespace

mesh::Mesh VoxelGrid::mesh_smooth(std::size_t level, SmoothOptions options) const {
    mesh::Mesh out;
    if (level >= levels_.size()) return out;
    const ChunkMap& chunks = levels_[level].chunks;
    if (chunks.empty()) return out;

    // The occupied box, plus the one-cell apron the mesher needs to close the
    // surface, plus one more per blur pass so a blurred sample near the edge
    // reads the same zeros an unblurred one would.
    const int pad = 2 + std::max(options.blur, 0);
    VoxelCoord lo{0, 0, 0}, hi{0, 0, 0};
    bool first = true;
    for (const auto& [key, chunk] : chunks) {
        if (chunk.occupied <= 0) continue;
        const VoxelCoord c0{key.x * kChunkDim, key.y * kChunkDim, key.z * kChunkDim};
        const VoxelCoord c1{c0.x + kChunkDim - 1, c0.y + kChunkDim - 1, c0.z + kChunkDim - 1};
        if (first) {
            lo = c0;
            hi = c1;
            first = false;
            continue;
        }
        lo = {std::min(lo.x, c0.x), std::min(lo.y, c0.y), std::min(lo.z, c0.z)};
        hi = {std::max(hi.x, c1.x), std::max(hi.y, c1.y), std::max(hi.z, c1.z)};
    }
    if (first) return out;

    // The box is the occupied CHUNKS' extent, which is what the grid actually
    // holds. A grid whose material sits far apart on two axes makes this box
    // large — the same shape mesh_greedy's slab grouping exists to avoid — so
    // the span is checked in 64-bit before anything is allocated, rather than
    // overflowing an int and asking for a buffer no allocator can satisfy.
    OccupancyBox box;
    long long total = 1;
    for (int a = 0; a < 3; ++a) {
        const long long l = (a == 0 ? lo.x : a == 1 ? lo.y : lo.z) - pad;
        const long long h = (a == 0 ? hi.x : a == 1 ? hi.y : hi.z) + pad;
        const long long span = h - l + 1;
        if (span <= 0 || span > (1 << 20)) return out;
        box.min[a] = static_cast<int>(l);
        box.dim[a] = static_cast<int>(span);
        total *= span;
        if (total > (1LL << 28)) return out;  // ~256M samples; a byte over a GiB of floats
    }
    box.value.assign(static_cast<std::size_t>(total), 0.0f);
    for (int z = box.min[2]; z < box.min[2] + box.dim[2]; ++z)
        for (int y = box.min[1]; y < box.min[1] + box.dim[1]; ++y)
            for (int x = box.min[0]; x < box.min[0] + box.dim[0]; ++x)
                if (cell_at(level, {x, y, z}) != 0) box.value[box.index(x, y, z)] = 1.0f;

    for (int i = 0; i < options.blur; ++i) blur_once(box);

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
