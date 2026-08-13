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

#include <functional>

#include "clay/mesh/marching.h"  // MeshingOptions
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
