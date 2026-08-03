#pragma once

// Dual contouring (meshing spec, flagged): Hermite data — crossing position
// plus field normal per sign-changing lattice edge — feeds a per-cell QEF
// solve (regularized 3x3 normal equations biased toward the crossing
// centroid, vertex clamped into its cell), so vertices land on sharp
// features instead of rounding them like marching or surface nets.
//
// EXPERIMENTAL: per the meshing spec, dual contouring ships behind the
// explicit opt-in flag DualContouringOptions::enable_experimental until
// hardened post-v1 — plain DC is not guaranteed manifold. mesh_tape_dc
// returns an empty mesh unless the flag is set.

#include <functional>

#include "clay/mesh/marching.h"  // MeshingOptions + attribute helpers
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace mesh {

struct DualContouringOptions {
    MeshingOptions attributes;         // vertex color/normal pass (as mesh_tape)
    bool enable_experimental = false;  // meshing spec: DC ships flagged
    float qef_lambda = 1e-2f;          // mass-point regularization strength
};

// Core lattice mesher with Hermite normals from central differences of the
// sampler (the sampler must be valid one lattice point beyond the cell
// range). Same open-boundary behavior as mesh_lattice_nets. Not gated by the
// flag: callers reaching for the lattice API opted in by construction.
Mesh mesh_lattice_dc(const std::function<float(int, int, int)>& sample, const int cell_min[3],
                     const int cell_max[3], kernel::cfloat3 origin, float spacing,
                     float qef_lambda = 1e-2f);

// Mesh a tape over a world region; Hermite normals come from the tape
// gradient. Closes region-crossing geometry against an out-of-range positive
// ring, like mesh_tape. Returns {} unless options.enable_experimental.
Mesh mesh_tape_dc(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
                  const DualContouringOptions& options = {});

}  // namespace mesh
}  // namespace clay
