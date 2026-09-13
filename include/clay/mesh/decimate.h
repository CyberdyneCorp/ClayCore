#pragma once

// Quadric edge-collapse decimation via meshoptimizer (meshing spec):
// target triangle ratio or error bound, vertex-color aware — collapses
// respect color boundaries through attribute weighting.
//
// A MANIFOLD INPUT YIELDS A MANIFOLD RESULT, and that is checked rather than
// assumed. meshoptimizer applies its own collapse rules and not the link
// condition `collapse_edge` refuses on (mesh/topology_ops.h), so a simplified
// mesh can carry edges with four incident triangles -- measured on 4 of 20
// shape-and-ratio configurations. Where that happens the simplification is
// retried, preferring a different choice of collapses at the requested size
// over a larger result; where no retry is clean the INPUT is returned, because
// a caller that asked for fewer triangles is better served by more of them than
// by a surface it cannot use. A mesh that arrives non-manifold is simplified and
// returned as before.
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
