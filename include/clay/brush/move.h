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
// UNDER SYMMETRY, REFLECT THE BRUSH, NOT THE BOUND (#363). The compiler emits
// a mirrored item as itself plus one copy per axis, and a copy's field at p is
// the item's whole record — deformer chain included — evaluated at the
// reflected point. So a grab in an item's chain moves the item near the ball
// AND its copy near the ball's reflection, and an item whose COPY sits under
// the ball has its body at the reflection, where a grab centred on the ball
// weighs zero: measured on a ball at (-1,-.3,0) whose copy sat under a drag at
// (1.1,0,0), the copy's pole moved by 0.00000 while everything else moved by
// -0.05945. Selecting on the mirror-expanded influence bound made that the
// common case — every participating item's bound spans the plane, so a grab
// on a ridge at x 1.45 took 46 items where the unmirrored drag took 22, the
// base among them, each a warp that did nothing — and left the sculpt
// asymmetric between a drag and its mirror image (5,536 samples differing).
//
// So the drag is stated as the set of IMAGES the layer's symmetry would emit
// of it — the ball, then one reflection per mirror axis, then one rotation per
// radial copy; additive, exactly as emit_item copies the item — and each item
// is tested on its OWN bound against each image. An image that reaches the
// item yields a grab at that image's centre with that image's displacement:
// the reflected ball grabs the items whose reflections sit under the ball.
// Items that do not participate in the symmetry see the ball alone. With no
// symmetry there is one image and this is byte-for-byte the rule it replaces.
//
// ONE WARP PER NODE, CARRYING ONE GRAB PER REACHING IMAGE. An item both images
// reach — one straddling the plane — takes both, as two brushes would, and a
// drag centred on the plane gives it two grabs of opposite pull that compose
// to a pinch, which is continuous as the centre approaches the plane and is
// what a mesh sculptor's mirror does. The grabs are ordered by their VALUES,
// never by which image produced them: once the item is not itself
// plane-symmetric the two orders are different fields, and the order has to
// come out the same whichever side the drag was made from for the +x drag to
// be the mirror image of the -x one — which it is, bit for bit, on an
// identity layer transform.
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

// One image of a drag under the layer's symmetry: where the ball is and which
// way it pulls, in WORLD space.
struct DragImage {
    kernel::cfloat3 centre;
    kernel::cfloat3 displacement;
};

// The images the layer's symmetry makes of a drag, in the order the compiler
// emits copies: the drag itself FIRST, then one reflection per set mirror
// axis, then one rotation per radial copy — `1 + popcount(mirror_axes) +
// (radial_count - 1)` images, additive, never products (x|y is two
// reflections, not four quadrants). Stated in the layer's local frame and
// mapped back, so on an identity layer transform a reflection is an exact
// sign flip. With no symmetry the one image is the drag, untouched.
//
// Exposed because the gesture's REACH is the union of these balls: the copies
// move where the images are, and a host that invalidates the ball alone
// serves the reflected side stale.
std::vector<DragImage> drag_images(const scene::Layer& layer, kernel::cfloat3 world_centre,
                                   kernel::cfloat3 world_displacement);

// One item's share of the drag: the grabs that reproduce it in that item's own
// frame, one per image of the drag that reaches the item — usually one, two
// for an item straddling a mirror plane. Already in the order the chain takes
// them. They belong at the FRONT of the node's chain — see `moved_chain`.
struct MoveWarp {
    scene::NodeId node = scene::kNoNode;
    std::vector<scene::Deformer> deformers;
    // The REST of the gesture's identity on this item: one grab per image the
    // item can see and that does not reach it this frame. `moved_chain`
    // recognises its own earlier frames by `deformers` and these together, so
    // an image that reached the item while the pull was wider (a grab dilates
    // the bound by its displacement) and no longer does is replaced with the
    // rest of the gesture instead of left behind with a stale pull. Empty for
    // every item under no symmetry, which is what keeps the resolver at one
    // allocation per reached item there; a caller building a warp by hand may
    // repeat `deformers` here, since the match reads both.
    std::vector<scene::Deformer> gesture;
};

// One image of the drag as ONE item sees it: where that image's ball lands in
// the item's own scaled-local frame, whether it reaches the item, and — for a
// copy — the reflection or rotation that makes the image's displacement out of
// the drag's.
struct PreparedImage {
    kernel::cfloat3 local_centre = kernel::cf3(0, 0, 0);
    // The map the compiler applies in the LAYER's frame to emit the copy this
    // image stands for: a reflection or a rotation, applied to the drag's
    // displacement the way `drag_images` applies it to the drag. Left at the
    // identity, and never read, for the drag itself.
    math::cfloat4x4 linear = math::identity_matrix();
    // A copy the layer's symmetry emits, rather than the drag itself, whose
    // displacement is the world displacement untouched.
    bool copy = false;
    // Whether this image's ball reaches the item's OWN bound. At least one
    // image of a prepared item does; an item no image reaches is not prepared.
    bool reaches = false;
};

// One affected item's share of a drag, resolved as far as it can be BEFORE the
// displacement is known.
//
// A drag holds its anchor and its radius fixed for the whole gesture and only
// grows the displacement. Everything that depends on the anchor and the radius
// — which items the drag reaches, through which of its images, where each
// image's centre lands in the item's own frame, what the radius becomes there
// — is therefore decided once, at pointer down, and only the displacement is
// per frame. Without this a live drag walks the entire SDF tree once per
// pointer event to rediscover an answer that cannot have changed.
//
// The transform data is kept as the terms `move_brush` already divides and
// rotates by, not as pre-inverted equivalents: a reciprocal multiplied is not a
// division, and a preview that differs from its commit in the last bits is a
// preview of something else. The same holds for a copy's displacement, which
// is reflected or rotated in the layer's frame and mapped back exactly as
// `drag_images` does it — so the layer transform travels with the item.
struct PreparedMove {
    scene::NodeId node = scene::kNoNode;

    // The images of the drag this item can see, in `drag_images` order: the
    // drag itself first, then the copies the layer's symmetry emits. One,
    // reaching, under no symmetry; one for an item the compiler emits once.
    std::vector<PreparedImage> images;

    // The reach in this item's own scaled-local frame, shared by every image.
    float local_radius = 0.0f;

    // What turns a world DISPLACEMENT into this item's local one: the rotation
    // and the scale, but not the translation, and the per-axis scale last
    // because it is innermost.
    math::Quat inverse_rotation = math::Quat::identity();
    float world_scale = 1.0f;
    kernel::cfloat3 scale_axes = kernel::cf3(1.0f, 1.0f, 1.0f);

    // The layer's own transform, under which a copy's displacement is
    // reflected or rotated before it is mapped into the item.
    math::Transform layer_xform;

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

// The other half: the warp for a TOTAL world displacement — one grab per image
// that reaches the item, ordered by value, and the rest of the gesture's
// identity in `gesture`. O(images), and no scene access at all — which is
// what makes a live drag cost the items it moves.
//
// TOTAL, from the anchor, never an increment on the last frame: a chain of
// increments composes warps that were each authored against a different
// intermediate surface, and the result is not the drag the artist made. The
// same reason `moved_chain` REPLACES a leading grab from the same drag rather
// than stacking on it.
MoveWarp resolve_prepared_move(const PreparedMove& prepared,
                               kernel::cfloat3 total_world_displacement);

// The same, into a warp the caller keeps from frame to frame. A warp carries
// its grabs in vectors, and a live drag that took a fresh one per item per
// frame paid the allocator as much as the arithmetic (measured 2.4x on the
// per-frame row); reused, the vectors keep their capacity after the first
// frame and a frame costs the items it moves. `out` is cleared first.
void resolve_prepared_move(const PreparedMove& prepared, kernel::cfloat3 total_world_displacement,
                           MoveWarp* out);

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

// Order a gesture's deformers on ONE item by their values — centre, then
// payload — descending, never by which of the layer's symmetry images produced
// them. Once the item is not itself plane-symmetric the two orders are two
// different fields, so a drag made from the +x side has to come out the mirror
// image of the same drag made from -x, bit for bit.
//
// Exposed because the magnify resolver (brush/magnify.h) needs the same
// determinism for the same reason, and one ordering rule in one place is what
// keeps the two gestures from drifting apart.
void order_by_value(std::vector<scene::Deformer>* deformers);

// The chain `node` should end up with: the gesture's deformers first, then
// whatever was there minus the leading deformers of this same gesture, one
// frame earlier. One place for the ordering rule, so a caller cannot get it
// subtly wrong.
//
// Serves a MAGNIFY gesture too (brush/magnify.h) without a second rule: an
// entry continues the gesture when it matches one of the gesture's images by
// KIND, centre and radius, so a pinch replaces its own last frame and leaves a
// drag's grab over the same ball alone.
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
