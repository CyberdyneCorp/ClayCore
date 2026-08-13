#include "clay/mesh/surface_nets.h"

#include "dual_grid.h"

namespace clay {
namespace mesh {

Mesh mesh_lattice_nets(const std::function<float(int, int, int)>& sample, const int cell_min[3],
                       const int cell_max[3], kernel::cfloat3 origin, float spacing) {
    return detail::dual_grid_mesh(sample, cell_min, cell_max, origin, spacing, {},
                                  detail::nets_vertex);
}

Mesh mesh_tape_nets(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
                    const MeshingOptions& options) {
    return detail::tape_nets_mesh(tape, region, voxel_size, options, /*keep_quads=*/false);
}

}  // namespace mesh
}  // namespace clay
