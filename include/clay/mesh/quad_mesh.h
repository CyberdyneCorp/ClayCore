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
// Four properties of the output, stated rather than discovered:
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
//  - SOME QUADS ARE VANISHINGLY SMALL. A cell whose vertex lands on top of a
//    neighbour's — which symmetry on a thin feature makes happen, the two
//    crossings of a wall averaging to the same point — leaves a quad with
//    near-zero area. On a thin capsule a handful of quads come out around
//    1e-15 of a unit against a median near 1e-4. This is INHERITED, not
//    introduced: the same cells produce the same degenerate TRIANGLES in
//    mesh_tape_nets at the same cell size, position for position. It is
//    called out here because a zero-area face is what a DCC importer warns
//    about first, and because a mesh validator will find it before the user
//    does. Nothing welds them away — collapsing a face moves vertices that
//    the field put where they are, and the triangle path has always shipped
//    them.
//
//  - THE OUTPUT IS NOT MANIFOLD AND NOT WATERTIGHT, for TWO reasons, only the
//    first of which mesh/surface_nets.h gives:
//      * a cell the surface crosses twice gets one vertex and the two sheets
//        pinch; and
//      * DIAGONAL occupancy, which a voxel sculpt produces constantly. Two
//        solid cells that meet only along a lattice EDGE put one quad edge in
//        FOUR quads, and there is no consistent winding across it; two that
//        meet only at a CORNER share one vertex between two otherwise
//        disjoint sheets, which is a bowtie. Both are the shape being what it
//        is rather than either mode getting it wrong — a diagonal solid has
//        no manifold surface to find — and a 4x4x4 checkerboard is nothing
//        but this case (108 four-quad edges in both modes). A caller who
//        needs a manifold from such a sculpt has to thicken the diagonal, not
//        change mesher.
//    Two is the count for THIS mesher. VoxelGrid's faces mode adds a third of
//    its own — its corner weld is keyed by palette index, so a painted sculpt
//    is split along every colour seam — and voxel/grid.h is where that one is
//    stated, next to the weld that causes it.
//    mesh_tape (marching) remains the watertight, 2-manifold export path and
//    is unaffected by any of this.
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

// mesh_tape_quads' own pricing, asked without meshing: would it lattice this
// region at this cell size, or refuse it as more than kMaxGridSamples sample
// points? False for a region it would not mesh at any cell size (empty or
// infinite) and for a cell size that is not finite and positive.
bool lattice_affordable(const math::Aabb& region, float cell_size);

// The finest cell size lattice_affordable still accepts for this region, given
// a `coarse` one it already accepts. Found by bisection rather than by solving
// the cubic: the sample count is monotone in the cell size, the halvings pin
// the bound well below a float ulp, and an inverted cubic would have to be
// checked against the pricing anyway to be trusted.
//
// The return value is AFFORDABLE, not merely near the bound — the bisection
// runs in double and narrowing to float rounds to nearest, so the last step is
// back up to a float the pricing accepts. That is the whole contract: this is
// the number the count search clamps to, and a floor mesh_tape_quads would
// refuse turns a resolution limit into an empty mesh. If `coarse` is itself
// refused there is no affordable cell size at all and `coarse` comes back
// unchanged.
float finest_affordable_cell(const math::Aabb& region, float coarse);

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
// NOR IS THE RESULT MONOTONIC IN THE TARGET. Each search starts from its own
// seed and stops after its own handful of meshes, so two nearby targets are
// two independent walks over a non-monotonic count: asking a voxel sphere for
// 4 quads can return more of them than asking for 10, because the smaller
// target's walk never visited the candidate the larger one landed on. A host
// wiring a slider to this should expect the count to move backwards
// occasionally; raising max_iterations makes it rarer and never rules it out.
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
    // The search wanted a lattice the source cannot give it and settled for
    // the nearest one it can. Over a continuous cell size (fit_quad_cell) that
    // is a limit: the fine end is the mesher's sample pricing (and, for
    // voxels, the grid's own voxel size), the coarse end a lattice too sparse
    // to span the region at all. Over a LADDER of fixed lattices
    // (fit_quad_ladder) it is the ladder's end: the target lies below what the
    // coarsest rung yields or above what the finest does. True even if the
    // clamped mesh then happened to land inside the tolerance: both statements
    // are then true.
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

// The same search over a LADDER instead of a continuum, for a source whose
// lattices are a fixed, ordered list rather than a number a secant can move —
// the voxel grid's level stack, where there is no cell size to step at all.
//
// Walks rung 0 (the coarsest, fewest quads) toward rung rungs-1 and STOPS at
// the first rung whose count reaches or passes the target: the count rises
// along the ladder, so no later rung can be nearer than the two this brackets
// the target between. It keeps the closest of the rungs it meshed by the same
// rule fit_quad_cell uses — never-empty over empty, then distance, then the
// candidate that does not exceed the target — and returns THAT mesh.
//
// The rising count is an ASSUMPTION ABOUT THE SOURCE, not a property of
// ladders, and it is what licenses the early stop. A source whose rungs are
// not ordered by count would have the walk stop at its first overshoot and
// report `clamped` while a nearer, unmeshed rung sat further along. The voxel
// stack satisfies it because a finer level resolves at least the faces a
// coarser one does; note that it is not a strict mip, so a scattered cloud of
// fine voxels leaves coarse rungs EMPTY — that stays sound only because the
// never-empty-over-empty rule above refuses to keep an empty rung, and because
// `clamped` below reads the COARSE END off the first rung that yields
// anything rather than off rung 0. An empty rung's count is 0, and 0 exceeds
// no target, so measuring the coarse end at rung 0 would report a ladder whose
// every usable rung overshoots as if it bracketed the target.
//
// max_iterations does NOT apply here, and that is the one place this parts
// company with fit_quad_cell. It exists there because a secant over a
// continuum has no natural end and every step costs a whole dense mesh. A
// ladder ends where the ladder ends, and the rungs are ordered coarse to fine,
// so the walk's total cost is dominated by its LAST rung — a stack whose count
// quadruples per level costs about a third more to walk in full than to mesh
// its finest level once. Capping that would buy nothing and would cost the
// contract below: a cap that stopped the walk early would be a third meaning
// of `clamped`, or a silent third case with no flag at all.
//
// `clamped` says THE LADDER RAN OUT, which is the honest reading of a fixed
// list: the target is below what the COARSEST RUNG THAT YIELDS ANYTHING gives
// or above what the last rung gives, and no rung of this source is nearer than
// the end that was returned. "Yields anything" and not "rung 0" because empty
// coarse rungs are ordinary on a voxel stack, as above: on a ladder whose
// counts run 0, 0, 1728, a target of 10 is off the coarse end even though rung
// 0 reports 0. An exact hit is neither end — the rung's count equals the
// target, so nothing overshot and nothing ran out.
// A target falling BETWEEN two rungs is NOT clamped even when it lands far
// off — both neighbours were meshed and the nearer one is the answer, which is
// what within_tolerance = false says. A ladder step is typically a factor of
// four in count, so within_tolerance is unreachable for most targets and this
// distinction is the whole of what the report can tell a caller.
//
// `iterations` is the rungs meshed. The walk always starts at rung 0 and
// charges every rung it passes, so a target met at rung k costs k+1 meshes,
// not the two the bracketing pair suggests — the rungs below the bracket were
// meshed to get there. It is bounded by the ladder's length, and that length
// is what a caller budgeting a slider must price against.
//
// `out_rung` receives the rung the returned mesh came from. A ladder has no
// cell size of its own, so QuadFit::cell_size comes back 0 and it is the
// caller that fills in what the rung is called — a voxel level's own voxel
// size, say. That is the one field of the report it must supply. A target of 0
// has no meaning here: there is no seed rung to echo, so the report comes back
// zeroed and no mesh is built. A caller with no target meshes its own default
// rung directly instead.
QuadFit fit_quad_ladder(const std::function<Mesh(std::size_t)>& mesh_at, std::size_t rungs,
                        const QuadTarget& target, std::size_t* out_rung, Mesh* out_mesh);

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
