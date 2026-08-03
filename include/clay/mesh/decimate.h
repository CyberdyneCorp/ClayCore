#pragma once

// Quadric edge-collapse decimation via meshoptimizer (meshing spec):
// target triangle ratio or error bound, vertex-color aware — collapses
// respect color boundaries through attribute weighting.

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
