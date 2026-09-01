#pragma once

// WHAT AN OPERATION COSTS BEFORE IT IS RUN (sculpt-runtime spec,
// add-extreme-poly-runtime).
//
// `MultiresSurface::preflight_add_level` already did this for one operation and
// was already the right shape: persistent and peak reported apart, a typed
// refusal, no side effects, no allocation. Four more operations have the same
// property — their transient PEAK exceeds the size of their result — and had no
// answer at all:
//
//   - converting a flat mesh into an adaptive surface, which holds the weld map
//     and both representations at once;
//   - exporting an adaptive surface back to a flat mesh, which holds the export
//     copy beside the surface;
//   - a global remesh, which holds the source and the target together;
//   - serializing a large surface, where the blob is a second copy of
//     everything.
//
// ONE ESTIMATOR UNDERNEATH ALL OF THEM. Five bespoke ones is five places for
// `vertices * bytes_per_vertex` to wrap and report a SMALL number, and the
// failure mode of that bug is that the operation is ALLOWED — which is the
// precise outcome the requirement exists to prevent. `memory::CapacityBuilder`
// checks every multiply and add and latches on overflow, so an estimate nobody
// can compute refuses instead of passing.
//
// THESE ARE CEILINGS, and deliberately so. The structural figures are exact
// byte costs, but the arrays that carry them are grown rather than reserved, so
// a measured-after-the-fact figure includes capacity slack the prediction does
// not. A budget that errs LOW is the one that gets an application killed: it
// says yes to an operation that does not fit. Where a slack factor is applied
// it is the one `multires.cpp` measured — 1.75 over a level-3 hierarchy — and
// it is applied to the prediction rather than left for a caller to remember.

#include <cstdint>

#include "clay/memory/capacity.h"

namespace clay {
namespace mesh {

struct Mesh;
class DynamicSurface;
class MultiresSurface;

// The shared answer. `MultiresPreflight` keeps its own name and fields — it is
// shipped ABI — and is filled from the same arithmetic.
struct SurfacePreflight {
    // What is left after the call, split by what a host may release.
    std::uint64_t authoritative_bytes = 0;
    std::uint64_t runtime_bytes = 0;
    std::uint64_t persistent_bytes = 0;
    // The high-water mark DURING the call. The number that matters on a device
    // that kills an app rather than warning it.
    std::uint64_t peak_bytes = 0;

    bool allowed = true;
    memory::BudgetError error = memory::BudgetError::None;
};

// `budget` of zero means no budget, which is what a desktop host passes and
// what every one of these did before there was a budget to pass. An overflow
// refuses at any budget, including none.

// A flat mesh becoming an adaptive surface. The peak holds the source mesh, the
// half-edge structure and the weld map at once.
SurfacePreflight preflight_to_dynamic(const Mesh& mesh, std::uint64_t budget = 0);

// An adaptive surface becoming a flat mesh. The export SPLITS a geometric
// vertex into as many export vertices as it has distinct corner attributes, so
// the result is bounded by corners rather than by vertices — which is the term
// that makes this bigger than it looks on a seam-heavy model.
SurfacePreflight preflight_to_mesh(const DynamicSurface& surface, std::uint64_t budget = 0);

// A global remesh to a target triangle count. Source and target are live at the
// same time, which is the whole reason this one is asked.
SurfacePreflight preflight_global_remesh(const Mesh& mesh, std::uint64_t target_triangles,
                                         std::uint64_t budget = 0);

// Serialization. The blob is a second copy of everything the surface holds, and
// it exists while the surface still does.
SurfacePreflight preflight_encode(const DynamicSurface& surface, std::uint64_t budget = 0);
SurfacePreflight preflight_encode(const MultiresSurface& surface, std::uint64_t budget = 0);

}  // namespace mesh
}  // namespace clay
