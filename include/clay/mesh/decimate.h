#pragma once

// Quadric edge-collapse decimation via meshoptimizer (meshing spec):
// target triangle ratio or error bound, vertex-color aware — collapses
// respect color boundaries through attribute weighting.
//
// DECIMATION CAN PINCH A SURFACE, AND THIS SAYS WHEN IT DID.
//
// meshoptimizer applies its own collapse rules and not the link condition
// `collapse_edge` refuses on (mesh/topology_ops.h), so a watertight 2-manifold
// input can come back with edges carrying four incident triangles.
//
// Where the requested simplification pinches, it is retried with a different
// choice of collapses AT THE SAME TARGET, and a clean result is returned in
// preference -- but only if it did not grow the mesh, because a caller picks a
// ratio because it needs that size. That recovers the cases where a
// pinch is incidental: measured over 20 shape-and-ratio configurations, 4
// pinched and all 4 recovered on the first retry within two triangles of the
// requested count.
//
// AT AGGRESSIVE RATIOS IT IS NOT RECOVERABLE AND THE RESULT IS RETURNED PINCHED.
// Asking for one triangle in twelve of a grouped model merges sheets because
// that is what the ratio means, not because a collapse went wrong: on
// examples/37_groups, 155,388 triangles to 12,418, every one of six retries
// came back pinched. Refusing that result would mean refusing to decimate, and
// handing back the 155,388-triangle input instead is not a service to a caller
// who asked for 8%.
//
// So pass a `DecimateReport` when the distinction matters -- an export gate,
// anything downstream that cannot take a non-manifold mesh -- and decide there.
// It is the caller who knows whether a smaller mesh or a two-sided one is worth
// more, and the decision is not one this can make for them.
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

struct DecimateReport {
    // No edge in the result carries more than two triangles.
    bool manifold = true;
    // Whether the INPUT was, which is what says whose defect a false `manifold`
    // is. An input that arrives pinched is simplified and returned as it always
    // was; this does not promise to repair what it did not break.
    bool input_manifold = true;
    // Simplifications actually run. 1 when the first result was clean.
    int attempts = 1;
};

Mesh decimate(const Mesh& m, const DecimateOptions& options, DecimateReport* report = nullptr);

}  // namespace mesh
}  // namespace clay
