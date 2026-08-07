#pragma once

// The Move brush (brush-engine spec, add-move-brush): dragging the assembled
// SURFACE, which is what ZBrush's Move does and what a `grab` deformer on its
// own does not.
//
// The deformation already existed. What did not is the step that turns a
// world-space drag into it, and there are three ways to get that wrong — the
// same reason cut_item and snakehook exist rather than leaving their geometry
// to each caller.
//
// A DEFORMER IS PER ITEM, AND LOCAL. Its centre is in the item's own frame, so
// a centre of (0,0,0) grabs a sphere sitting at world x = 1.5 and a centre of
// (1.5,0,0) does nothing to it. Nothing errors either way.
//
// SO GRABBING ONE ITEM OF A BLENDED FORM LEAVES THE REST BEHIND. On two
// smooth-unioned balls, a grab on the left lifts its side by 0.070 and the
// right by 0.000. A blocked-out sculpt is exactly that case, so the naive brush
// is wrong in the normal case rather than in an edge one.
//
// AND THE WARP MUST GO AT THE FRONT. `Node::deformers` applies in authoring
// order — the tape warps by `deformers[0]` first — so the first entry is the
// OUTERMOST warp on the geometry. One appended at the back has its region
// weight evaluated at a point the earlier deformers already moved, so the drag
// lands somewhere the user did not put it. Invisible until an item has two.
//
// Applying the same warp to every operand is not an approximation of warping
// their combination, it IS warping their combination: combine ops are pointwise
// in the deformed point, so op(f(W(p)), g(W(p))) == (op(f,g))(W(p)). And
// Transform's scale is uniform by design, so a spherical falloff stays
// spherical mapped into an item's frame instead of becoming an ellipsoid.
//
// INHERITED FROM GRAB, and not fixed here: the surface moves LESS than the
// displacement asked for, because the region weight is taken at the sample
// point rather than at its preimage — a drag of 0.5 over a radius of 0.8 moves
// a tip about 0.31. That is deliberate upstream (the true preimage costs an
// iteration per sample and buys nothing a sculptor can feel) and the pull is
// monotonic, so a UI can calibrate against it.

#include <cstdint>
#include <vector>

#include "clay/scene/document.h"
#include "clay/scene/types.h"

namespace clay {
namespace brush {

struct MoveSettings {
    // Radius of the drag, in WORLD units.
    float radius = 0.3f;
    // Falloff curve across the region, from the shared easing table.
    std::uint8_t ease = 0;
    // Gate the pull on the half-space it heads into, so the far side of a form
    // does not travel with the near side.
    bool front_only = false;
};

// One item's share of the drag: the grab that reproduces it in that item's own
// frame. It belongs at the FRONT of the node's chain — see `moved_chain`.
struct MoveWarp {
    scene::NodeId node = scene::kNoNode;
    scene::Deformer deformer;
};

// Resolve a world-space drag into the warps that reproduce it on the layer's
// assembled surface.
//
// PURE: the layer is read, never written, so a host can preview a drag before
// committing it. The warps are returned rather than applied for the reason the
// stroke engine returns nodes — the caller decides which commands carry them,
// and one SetDeformersCmd per node inside an undo group is what makes a whole
// drag one undo step.
//
// Items the drag cannot reach get no warp at all: a deformer with finite
// support, outside its own support, changes nothing and still costs a tape
// record on every evaluation. Groups themselves take no warp — their transform
// does not reach their children in this scene model, so the children are what
// carry it.
//
// Nothing comes back for a non-positive radius or a displacement of zero:
// neither describes a drag, and a chain of no-op deformers is worse than none.
std::vector<MoveWarp> move_brush(const scene::Layer& layer, kernel::cfloat3 world_centre,
                                 kernel::cfloat3 world_displacement,
                                 const MoveSettings& settings = {});

// The chain `node` should end up with: the move first, then whatever was there.
// One place for the ordering rule, so a caller cannot get it subtly wrong.
std::vector<scene::Deformer> moved_chain(const scene::Node& node, const MoveWarp& warp);

}  // namespace brush
}  // namespace clay
