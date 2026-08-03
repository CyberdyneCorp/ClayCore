#pragma once

// Default SDF mesher: marching tetrahedra over a Freudenthal 6-tet cube
// decomposition with globally consistent face diagonals. No ambiguous sign
// configurations exist, so output is watertight and 2-manifold by
// construction (meshing spec). Vertices are welded on canonical lattice-edge
// keys; triangle orientation is outward (toward positive field).

#include <cstdint>
#include <functional>

#include "clay/brick/cache.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace mesh {

enum class NormalMode : std::uint8_t {
    None,
    Face,      // area-weighted face normals
    Gradient,  // field gradient via the tape (blend-faithful)
};

struct MeshingOptions {
    NormalMode normals = NormalMode::Gradient;
    bool colors = true;         // sample the tape's color field per vertex
    float gradient_eps = 1e-4f;
};

// Core lattice mesher: sample(i,j,k) returns the field at lattice point
// (i,j,k); cells [cell_min, cell_max) are marched (cell (i,j,k) spans
// lattice points i..i+1 etc). World position = origin + index * spacing.
Mesh mesh_lattice(const std::function<float(int, int, int)>& sample, int cell_min[3],
                  int cell_max[3], kernel::cfloat3 origin, float spacing);

// Mesh a tape over a world region at the given voxel size (dense evaluation
// through the CPU reference; backends provide accelerated variants via
// Backend::mesh).
Mesh mesh_tape(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
               const MeshingOptions& options = {});

// Mesh a filled brick cache: marches only cells owned by surface bricks
// (the band width >= 1 voxel guarantees no crossing escapes into
// inside/outside bricks). Attributes come from the tape when given.
Mesh mesh_bricks(const brick::BrickCache& cache, const scene::Tape* tape_for_attributes,
                 const MeshingOptions& options = {});

// Attribute helpers (meshing spec: vertex attributes).
void apply_tape_attributes(Mesh& m, const scene::Tape& tape, const MeshingOptions& options);
void compute_face_normals(Mesh& m);
// Box-projection UVs from the dominant normal axis; scale = world units per
// UV tile.
void uv_box_project(Mesh& m, float scale = 1.0f);

}  // namespace mesh
}  // namespace clay
