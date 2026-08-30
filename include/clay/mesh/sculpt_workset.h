#pragma once

// The WORKSET: everything one stamp reads, and the record of what it wrote
// (meshing spec, add-shared-brush-kernels).
//
// A stamp costs what it touches. That rule is the whole reason this type is a
// reusable object rather than a set of locals: its arrays are sized to the
// largest footprint the stroke has seen and cleared through their own contents,
// never wholesale, so nothing here is ever proportional to the surface.
//
// THE READ HALO IS NOT THE WRITE REGION, and keeping them apart is the point of
// the second half of this file. Smooth, Relax and Polish average over a
// one-ring, so they READ vertices they never MOVE. A dirty report that named
// the halo would make a host re-upload geometry that did not change — on a
// multi-million-vertex surface at pointer rates, that is the difference between
// a preview that keeps up and one that does not.
//
// Shared rather than per-sculptor because the adaptive and multiresolution
// sculptors gather a workset too, and three answers to "what did this stamp
// touch" is three chances for a host to be told the wrong one.

#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/math/geom.h"
#include "clay/mesh/sculpt_common.h"

namespace clay {
namespace mesh {

// The PRE-STAMP SNAPSHOT. Everything a verb reads, captured before anything is
// written, which is what lets a composed verb be one operation:
//
//   - Draw takes ONE direction for the whole stamp from `average_normal`, so a
//     stroke does not chase its own deposit into a balloon.
//   - Smooth's Laplacian reads neighbours from here where they are in the
//     region, so it is a simultaneous average rather than a Gauss-Seidel sweep
//     whose result depends on vertex order.
//   - Scrape is flatten-cut-only AND smooth against these same positions, and
//     Crease is a draw AND a pinch against them. Calling the halves in
//     sequence is a different operation and a worse one — the same rule
//     `VoxelGrid::sculpt_scrape` already states.
struct SculptWorkset {
    std::vector<std::uint32_t> classes;      // weld classes the falloff reached
    std::vector<float> weights;              // the composed weight, in [0,1]
    std::vector<kernel::cfloat3> positions;  // pre-stamp, one per class
    std::vector<kernel::cfloat3> normals;    // pre-stamp, angle-weighted, unit

    // The automask factors, composed. Sized only when a stamp has any: an
    // automask must cost the workset and never the surface, and an empty vector
    // is how "this stamp has none" is said without a per-vertex branch.
    std::vector<float> automask;

    kernel::cfloat3 average_normal = kernel::cf3(0, 1, 0);
    kernel::cfloat3 centroid = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_point = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal = kernel::cf3(0, 1, 0);

    bool empty() const { return classes.empty(); }
    std::size_t size() const { return classes.size(); }

    // class -> index into `classes`, kNoClass outside the region. Sized to the
    // adjacency's class count and reset over `classes` alone, so a stamp costs
    // what it reached rather than what the mesh holds.
    //
    // It is also how the READ HALO is expressed: a ring neighbour whose slot is
    // `kNoClass` is one the stamp reads and does not move.
    std::vector<std::uint32_t> slot;

    // -- what the stamp actually WROTE ---------------------------------------
    //
    // A subset of `classes`: the ones whose displacement was not zero. The rim
    // of a falloff, a fully masked vertex and a verb that declined all leave
    // entries in `classes` that never move, and a host told about those would
    // upload them for nothing.
    std::vector<std::uint32_t> write_region;
    // The bounds of what moved, in the mesh's own space. Empty when nothing
    // did.
    math::Aabb write_bounds;

    // Clear the per-stamp arrays and KEEP their storage. `slot` is not cleared
    // here — it is a per-class array reset through `classes` by the gather,
    // which is the only reset that costs the footprint rather than the surface.
    void clear_keep_capacity() {
        classes.clear();
        weights.clear();
        positions.clear();
        normals.clear();
        automask.clear();
        write_region.clear();
        write_bounds = math::Aabb{};
    }
};

// The name this type had when it was only the fixed sculptor's. Kept so that
// nothing outside has to change, and because "the region under the brush" is
// still what it is.
using BrushRegion = SculptWorkset;

}  // namespace mesh
}  // namespace clay
