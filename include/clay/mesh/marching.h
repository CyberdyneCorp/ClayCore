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

// Ceiling on the dense sample grid a single mesh_tape call will allocate.
// The grid is sized from the caller's voxel size, so without a ceiling an
// over-fine size ends the process in the allocator rather than returning —
// the library builds without exceptions.
//
// Set from what the API PROMISES, not from what is comfortable: the documented
// headline call meshes at resolution 512, which is 514^3 = 136M lattice points
// over a cubic region. The ceiling has to clear that, so it is 2^28 (268M
// samples, about 1 GB of float) — a guard against a runaway request, not a
// working budget.
inline constexpr std::size_t kMaxGridSamples = 1u << 28;

// Mesh a tape over a world region at the given voxel size (dense evaluation
// through the CPU reference; backends provide accelerated variants via
// Backend::mesh). Returns an empty mesh for an empty or infinite region, for a
// voxel size that is not finite and positive, and for one so fine that the
// region needs more than kMaxGridSamples lattice points.
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
