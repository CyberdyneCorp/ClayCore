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

#include "clay/math/transform.h"
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

// One affected item's share of a drag, resolved as far as it can be BEFORE the
// displacement is known.
//
// A drag holds its anchor and its radius fixed for the whole gesture and only
// grows the displacement. Everything that depends on the anchor and the radius
// — which items the drag reaches, where its centre lands in each item's own
// frame, what its radius becomes there — is therefore decided once, at pointer
// down, and only the displacement is per frame. Without this a live drag walks
// the entire SDF tree once per pointer event to rediscover an answer that
// cannot have changed.
//
// The transform data is kept as the terms `move_brush` already divides and
// rotates by, not as pre-inverted equivalents: a reciprocal multiplied is not a
// division, and a preview that differs from its commit in the last bits is a
// preview of something else.
struct PreparedMove {
    scene::NodeId node = scene::kNoNode;

    // The anchor and the reach in this item's own scaled-local frame.
    kernel::cfloat3 local_centre = kernel::cf3(0, 0, 0);
    float local_radius = 0.0f;

    // What turns a world DISPLACEMENT into this item's local one: the rotation
    // and the scale, but not the translation, and the per-axis scale last
    // because it is innermost.
    math::Quat inverse_rotation = math::Quat::identity();
    float world_scale = 1.0f;
    kernel::cfloat3 scale_axes = kernel::cf3(1.0f, 1.0f, 1.0f);

    std::uint8_t ease = 0;
    bool front_only = false;
};

// What preparing a drag walked, so a scaling test has a number that does not
// depend on how fast the machine is. `visited` is the whole point: it is what
// must stay put per FRAME once preparation has happened, and it is what grows
// with unrelated model if a live drag is still traversing the tree.
struct MovePrepareStats {
    std::size_t visited = 0;  // nodes the traversal looked at, groups included
    std::size_t reached = 0;  // of those, the items the drag can actually reach
};

// The half of a drag that does not depend on how far it has gone: which items
// it reaches, and their frames. Empty for a non-positive radius or a layer with
// no edit list, exactly as `move_brush` is.
std::vector<PreparedMove> prepare_move(const scene::Layer& layer, kernel::cfloat3 world_centre,
                                       const MoveSettings& settings = {},
                                       MovePrepareStats* out_stats = nullptr);

// The other half: the warp for a TOTAL world displacement. O(1), and no scene
// access at all — which is what makes a live drag cost the items it moves.
//
// TOTAL, from the anchor, never an increment on the last frame: a chain of
// increments composes warps that were each authored against a different
// intermediate surface, and the result is not the drag the artist made. The
// same reason `moved_chain` REPLACES a leading grab from the same drag rather
// than stacking on it.
MoveWarp resolve_prepared_move(const PreparedMove& prepared,
                               kernel::cfloat3 total_world_displacement);

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

// The same rule against a CHAIN rather than a node, for a caller holding the
// pre-stroke chain by value — which a live drag must, since the node in the
// document is the one thing it has promised not to touch. The overload above
// is this one applied to `node.deformers`, so there is one ordering rule and
// not two.
std::vector<scene::Deformer> moved_chain(const std::vector<scene::Deformer>& chain,
                                         const MoveWarp& warp);

}  // namespace brush
}  // namespace clay
