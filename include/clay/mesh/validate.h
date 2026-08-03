#pragma once

// Mesh validation (meshing spec): watertightness, edge-manifoldness,
// orientation consistency, degenerate detection, and sampled
// self-intersection — the primitive behind CI export gates and any
// "guaranteed clean geometry" claim.

#include <cstdint>

#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

struct ValidationReport {
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    bool watertight = false;   // every edge shared by exactly two triangles
    bool manifold = false;     // no edge with more than two incident triangles
    bool oriented = false;     // the two triangles of every edge disagree in direction
    std::size_t boundary_edges = 0;
    std::size_t non_manifold_edges = 0;
    std::size_t degenerate_triangles = 0;  // repeated indices within a triangle
    std::size_t sliver_triangles = 0;      // near-zero area (informational)
    std::size_t intersecting_pairs = 0;    // sampled self-intersection hits
    long long euler_characteristic = 0;    // V - E + F

    bool clean() const {
        return watertight && manifold && oriented && degenerate_triangles == 0 &&
               intersecting_pairs == 0;
    }
};

// max_intersection_pairs = 0 skips the (more expensive) sampled
// self-intersection pass; otherwise up to that many spatially-close,
// non-adjacent triangle pairs are tested exactly.
ValidationReport validate(const Mesh& m, std::size_t max_intersection_pairs = 0);

// Signed volume via divergence theorem — positive when triangle normals
// point outward (locks orientation conventions in tests).
double signed_volume(const Mesh& m);
double surface_area(const Mesh& m);

}  // namespace mesh
}  // namespace clay
