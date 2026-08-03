#include "clay/mesh/surface_nets.h"

#include "dual_grid.h"

namespace clay {
namespace mesh {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// Gibson surface nets vertex: centroid of the cell's edge crossings (always
// inside the cell — it is a convex combination of points on the cell).
cfloat3 nets_vertex(const detail::DualCrossing* crossings, int count, cfloat3 lo, cfloat3 hi) {
    if (count == 0) return (lo + hi) * 0.5f;  // unreachable for sign-changing cells
    cfloat3 sum = cf3(0, 0, 0);
    for (int i = 0; i < count; ++i) sum = sum + crossings[i].pos;
    return sum / static_cast<float>(count);
}

}  // namespace

Mesh mesh_lattice_nets(const std::function<float(int, int, int)>& sample, const int cell_min[3],
                       const int cell_max[3], kernel::cfloat3 origin, float spacing) {
    return detail::dual_grid_mesh(sample, cell_min, cell_max, origin, spacing, {}, nets_vertex);
}

Mesh mesh_tape_nets(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
                    const MeshingOptions& options) {
    detail::TapeGrid grid = detail::eval_tape_grid(tape, region, voxel_size);
    if (!grid.ok) return {};
    auto sample = [&](int i, int j, int k) { return grid.at(i, j, k); };
    // one extra cell ring: out-of-range samples are positive, so geometry
    // crossing the region boundary is closed (same ring as mesh_tape)
    int cmin[3] = {-1, -1, -1};
    int cmax[3] = {grid.nx, grid.ny, grid.nz};
    Mesh m = mesh_lattice_nets(sample, cmin, cmax, region.min, voxel_size);
    apply_tape_attributes(m, tape, options);
    return m;
}

}  // namespace mesh
}  // namespace clay
