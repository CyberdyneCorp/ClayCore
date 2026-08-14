#pragma once

// Quad meshing (meshing spec): keep the quads the dual mesher already builds
// instead of throwing them away at the triangulation.
//
// WHAT THIS IS NOT — read this before reaching for it. This produces a
// REGULAR QUAD GRID DERIVED FROM A SAMPLING LATTICE. It is NOT field-aligned
// retopology. The quads follow the lattice, not the form: there are no edge
// loops running around a limb or a mouth, no poles placed at features, no
// denser rows where curvature asks for them, and the result is NOT
// animation-ready — deforming it pinches wherever the topology disagrees with
// the shape, which is everywhere. This is the input a retopology pass
// REPLACES, not the output one produces. A caller expecting ZRemesher,
// QuadRemesher or Instant Meshes output should learn that here rather than
// from the mesh.
//
// What it IS good for: getting quads into a DCC that prefers them,
// subdividing a sculpt, and exporting a voxel model as the box faces it is.
//
// Three properties of the output, stated rather than discovered:
//
//  - VALENCE AVERAGES FOUR AND IS NOT FOUR EVERYWHERE. One vertex per surface
//    cell and one quad per crossing lattice edge makes the two counts equal,
//    so the mean is four — but a cell the surface enters through a corner has
//    six crossing edges and belongs to six quads, and one clipped by a corner
//    belongs to three. On a sphere it is roughly 55% four with the rest at
//    three, five and six, and that does not improve with resolution. What the
//    dual does guarantee is the property greedy MERGING cannot: no vertex ever
//    lands in the interior of another face's edge, so there are no
//    T-junctions to crack under subdivision.
//
//  - THE QUADS ARE NOT PLANAR. The four cell vertices around a lattice edge
//    are placed independently and nothing makes them coplanar, so an
//    application that triangulates on its own terms may shade a face
//    differently than this library's triangulation does. Nothing is
//    planarised: flattening a face moves its vertices off the surface, which
//    trades a shading artifact for a geometric error.
//  - THE OUTPUT IS NOT MANIFOLD AND NOT WATERTIGHT, for the reason
//    mesh/surface_nets.h already gives — a cell the surface crosses twice
//    gets one vertex and the two sheets pinch. mesh_tape (marching) remains
//    the watertight, 2-manifold export path and is unaffected by any of this.
//
// The quads ride in Mesh::quads BESIDE the triangles, never instead of them;
// mesh/mesh_data.h states that invariant, and quads_consistent() below is how
// a boundary asserts it.

#include <cstddef>
#include <functional>

#include "clay/mesh/marching.h"  // MeshingOptions + kMaxGridSamples
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace mesh {

// The lattice dual with its quads kept — mesh_lattice_nets' mesh, vertex for
// vertex and index for index, plus Mesh::quads. Same lattice conventions, same
// open boundary rule: a quad needs all four cells around its edge inside
// [cell_min, cell_max), so a caller wanting closed output pads the range with
// positive samples.
Mesh mesh_lattice_quads(const std::function<float(int, int, int)>& sample, const int cell_min[3],
                        const int cell_max[3], kernel::cfloat3 origin, float spacing);

// Quad-mesh a tape over a world region: mesh_tape_nets at the same cell size,
// with the quads kept. Identical positions and triangles to that call — the
// same sampler, the same closing ring of out-of-range positive samples, the
// same attributes — because it IS that call with one flag set.
//
// Returns an empty mesh for an empty or infinite region, for a cell size that
// is not finite and positive, and for one so fine that the region would need
// more than kMaxGridSamples lattice points (the pricing every dense mesher
// here owes its allocator).
Mesh mesh_tape_quads(const scene::Tape& tape, const math::Aabb& region, float cell_size,
                     const MeshingOptions& options = {});

// -- asking for a quad COUNT --------------------------------------------------
//
// The lattice cell size is the only lever on how many quads come out, so a
// requested count is a short SEARCH over cell size. Read what that can and
// cannot promise before wiring a slider to it.
//
// THE TARGET IS A HINT WITH A REPORTED ACTUAL. It is not a ceiling and it is
// not an exact count. A ceiling cannot be promised because the count is NOT
// monotonic in cell size: a finer lattice can resolve a thin feature the
// coarser one missed entirely and so ADD surface, which is exactly the case a
// bisection assuming monotonicity steps past. Where two candidates are equally
// close the search prefers the one that does not exceed the target, so the
// usual reading of "about this many" tends to hold — without being promised.
//
// GRANULARITY. The count goes as cell^-2, so a 1% change in cell size moves it
// about 2%: landing inside roughly 5-10% of a target is the expectation, the
// default tolerance is 10%, and a tolerance much below about 2% will exhaust
// the iteration cap and come back with the best attempt and
// within_tolerance = false. No rounding trick fixes that — the count is how
// many lattice edges the surface crosses.
//
// COST. EVERY ITERATION IS A WHOLE MESH, including a whole dense field
// evaluation on the tape path. max_iterations is therefore a cost knob rather
// than a quality knob, its default is deliberately small, and a caller who
// sets it to 20 has asked for twenty meshes.
struct QuadTarget {
    std::size_t target = 0;      // 0: no search — mesh once at the cell size given
    float tolerance = 0.10f;     // fraction of the target; <= 0 means 0.10
    int max_iterations = 4;      // meshes the search may build; <= 0 means 4
    // A floor the CALLER imposes, on top of the one the mesher's own pricing
    // imposes. 0 leaves the pricing ceiling as the only limit.
    float min_cell_size = 0.0f;
};

// What the search actually did, which is the only way a caller learns that a
// request for 50,000 produced 31,000 because a limit stopped it.
struct QuadFit {
    float cell_size = 0.0f;       // the lattice the returned mesh was built on
    std::size_t quad_count = 0;   // what it actually produced
    int iterations = 0;           // meshes the SEARCH built; 0 when none ran
    bool within_tolerance = false;
    // The search wanted a cell size outside the limits and meshed at the limit
    // instead — the fine end being the mesher's sample pricing (and, for
    // voxels, the grid's own voxel size), the coarse end being a lattice too
    // sparse to span the region at all. True even if the clamped mesh then
    // happened to land inside the tolerance: both statements are then true.
    bool clamped = false;
};

// The search itself, over any mesher that takes a cell size and returns a
// mesh — the tape path and the voxel path are both this function, so the two
// cannot drift into two different contracts.
//
// Steps on cell' = cell * sqrt(actual / target), the secant this relationship
// implies in log-log, with the per-step ratio capped to [0.5, 2] so one noisy
// count cannot throw the next lattice across the whole range. Keeps the best
// candidate seen and returns THAT mesh, never the last one tried. A candidate
// with no quads at all never wins over one with some: a target so coarse that
// the shape's topology collapses comes back as the best real mesh with
// within_tolerance = false, rather than as the collapse.
//
// seed_cell, min_cell and max_cell must be finite and positive with
// min_cell <= max_cell; the seed is clamped into that range before the first
// mesh. With target.target == 0 exactly one mesh is built, at the seed, and
// the report echoes it with zero iterations and within_tolerance true —
// nothing was asked for, so nothing was missed.
QuadFit fit_quad_cell(const std::function<Mesh(float)>& mesh_at, float seed_cell, float min_cell,
                      float max_cell, const QuadTarget& target, Mesh* out_mesh);

// mesh_tape_quads with a target: the same mesh at the cell size the search
// settles on, plus the report of how it got there.
//
// `cell_size` is the seed when it is positive. When it is not, the seed is
// estimated from the region's own surface area and the target — the count is
// area / cell^2 to a good approximation, and the box is the only area known
// before a mesh exists. With neither a cell size nor a target there is nothing
// to mesh at: the result is an empty mesh and a zeroed report.
//
// The fine limit is the cell size at which the region would need more than
// kMaxGridSamples lattice points — the search stops there and reports
// `clamped` rather than asking for a lattice mesh_tape_quads would refuse and
// reading the refusal as a shape that vanished. The coarse limit is one cell
// across the region's largest extent.
Mesh mesh_tape_quads_fit(const scene::Tape& tape, const math::Aabb& region, float cell_size,
                         const QuadTarget& target, const MeshingOptions& options = {},
                         QuadFit* out_fit = nullptr);

// The invariant mesh/mesh_data.h states, checked: either there are no quads,
// or the triangles are exactly their two-triangle expansion in order, over
// indices that all address a vertex. What the mesh stream asserts about bytes
// it did not write, and what the tests assert instead of restating it.
bool quads_consistent(const Mesh& m);

// What an operation that rewrites `indices` calls. Named rather than spelled
// `m.quads.clear()` at every call site so the rule is greppable: a stale quad
// list describes triangles that no longer exist and can be saved into a
// document before anyone notices.
void drop_quads(Mesh& m);

}  // namespace mesh
}  // namespace clay
