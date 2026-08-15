#pragma once

// One cage over a layer (brush-engine spec, lattice-gizmo): ZBrush's Gizmo
// Lattice acts on the whole subtool, and a claycore lattice deformer is per
// item, in that item's own frame.
//
// This is the step between, and it is the same step `brush::move_brush` is —
// one world-space gesture resolved into the per-item warps that reproduce it.
// Without it, placing a cage over a blocked-out form means authoring one cage
// per item, each in a different frame, and keeping them in step by hand.
//
// WHY THE DEFORMER NEEDED A TRANSFORM FOR THIS. An item's frame can be
// ROTATED, and a lattice box is axis-aligned by construction — so a
// world-axis-aligned cage is simply not axis-aligned in a rotated item's local
// space, and there is no per-item box that reproduces it. Resampling the cage
// onto a per-item grid would approximate what `Deformer::lattice_transformed`
// does exactly, so the deformer carries the placement instead.
//
// REACHABILITY IS NOT THE MOVE BRUSH'S. `move_brush` skips items a drag cannot
// reach, because `grab` has finite support. A lattice does not: outside its box
// the displacement is CLAMPED rather than zero, so material out there travels
// rigidly with the nearest part of the cage. A gizmo cage therefore reaches
// EVERY item in the layer, which is what a gizmo means, and this returns a warp
// for each rather than inventing a cutoff. Only an untouched cage resolves to
// nothing.
//
// MESH LAYERS ARE NOT HERE. A mesh layer takes a cage directly and forward,
// through `mesh::Lattice`, with no inverse and no approximation. A gizmo
// spanning both resolves the SDF layers through this and the mesh layers
// through that; composing them is a host's call and there is no document-wide
// entry point yet to guess the shape of.

#include <vector>

#include "clay/kernel/shim.h"
#include "clay/math/transform.h"
#include "clay/scene/document.h"
#include "clay/scene/types.h"

namespace clay {
namespace brush {

// A cage placed in the world: where it sits, the box it spans in its OWN
// space, how many control points it has, and how far each has been dragged.
//
// The offsets are in the cage's own space and are what the artist DRAGGED —
// material travels with them, as it does on the mesh lattice.
struct GizmoCage {
    math::Transform placement;  // cage space -> world
    kernel::cfloat3 box_min = kernel::cf3(-1, -1, -1);
    kernel::cfloat3 box_max = kernel::cf3(1, 1, 1);
    int nx = 3, ny = 3, nz = 3;
    // nx*ny*nz drags, x-fastest: index (i, j, k) at (k*ny + j)*nx + i. Empty
    // means an untouched cage, which resolves to nothing.
    std::vector<kernel::cfloat3> offsets;

    // Control-point count after the deformer's own clamping, so a caller can
    // size `offsets` without duplicating the rule.
    std::size_t point_count() const;
    // Where a control point started, in the cage's own space and then in the
    // world — what a UI draws for the handles.
    kernel::cfloat3 rest(int i, int j, int k) const;
    kernel::cfloat3 world_rest(int i, int j, int k) const;
    bool is_identity() const;
};

// One item's share of the cage: the lattice that reproduces it in that item's
// own frame. It belongs at the FRONT of the node's chain — see `caged_chain`.
struct LatticeWarp {
    scene::NodeId node = scene::kNoNode;
    scene::Deformer deformer;
};

// Resolve a world-placed cage into the warps that reproduce it on the layer's
// items.
//
// PURE: the layer is read, never written, so a host can preview a cage before
// committing it. The warps are returned rather than applied for the reason the
// move brush returns them — the caller decides which commands carry them, and
// one SetDeformersCmd per node inside an undo group is what makes a whole
// gesture one undo step.
//
// Groups take no warp of their own: a group's transform does not reach its
// children in this scene model, so the children are what carry it.
std::vector<LatticeWarp> lattice_gizmo(const scene::Layer& layer, const GizmoCage& cage);

// The chain `node` should end up with: the cage first, then whatever was there.
// One place for the ordering rule, so a caller cannot get it subtly wrong —
// `Node::deformers` applies in authoring order, so the first entry is the
// OUTERMOST warp and a cage appended at the back would be evaluated at points
// the earlier deformers already moved.
std::vector<scene::Deformer> caged_chain(const scene::Node& node, const LatticeWarp& warp);

}  // namespace brush
}  // namespace clay
