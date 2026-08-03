#pragma once

// Surface nets preview mesher (meshing spec): classic Gibson surface nets —
// one vertex per sign-changing cell at the centroid of its edge crossings,
// one quad per sign-changing lattice edge connecting the four adjacent cell
// vertices, wound toward positive field. Far fewer vertices and triangles
// than the marching mesher at equal resolution, which is why it is the
// preview path. Unlike the marching mesher, surface nets does NOT guarantee
// manifold (or watertight) output: a cell crossed twice by the surface still
// gets a single vertex, pinching the sheets together. Use mesh_tape /
// mesh_bricks for export-grade geometry.

#include <functional>

#include "clay/mesh/marching.h"  // MeshingOptions + attribute helpers
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace mesh {

// Core lattice mesher; same lattice conventions as mesh_lattice. Quads are
// emitted only where all four cells adjacent to a crossing edge lie inside
// [cell_min, cell_max), so surfaces touching the range boundary stay open
// unless the caller pads the range with positive samples (mesh_tape_nets
// does).
Mesh mesh_lattice_nets(const std::function<float(int, int, int)>& sample, const int cell_min[3],
                       const int cell_max[3], kernel::cfloat3 origin, float spacing);

// Mesh a tape over a world region; closes geometry crossing the region
// boundary against an out-of-range positive ring, like mesh_tape.
Mesh mesh_tape_nets(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
                    const MeshingOptions& options = {});

}  // namespace mesh
}  // namespace clay
