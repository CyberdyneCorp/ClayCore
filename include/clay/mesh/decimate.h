#pragma once

// Quadric edge-collapse decimation via meshoptimizer (meshing spec):
// target triangle ratio or error bound, vertex-color aware — collapses
// respect color boundaries through attribute weighting.
//
// The result is a TRIANGLE mesh even when the input carried quads: an edge
// collapse breaks the quad pairing the first time it fires, so decimate drops
// Mesh::quads rather than returning a list describing triangles that no longer
// exist. Approaching a quad count means re-meshing at another lattice cell
// size (mesh/quad_mesh.h), not decimating.

#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

struct DecimateOptions {
    float target_ratio = 0.5f;   // fraction of input triangles to keep
    float target_error = 1e-2f;  // relative to mesh extents
    float color_weight = 1.0f;   // 0 disables color-aware collapse costs
};

Mesh decimate(const Mesh& m, const DecimateOptions& options);

}  // namespace mesh
}  // namespace clay
