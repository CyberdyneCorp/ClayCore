#pragma once

// The Magnify/Pinch brush on a layer's assembled SURFACE (brush-engine spec,
// add-magnify-surface-brush).
//
// This exists for exactly the reason `move_brush` does, and the header there
// states the hazard at length: `cdeform_magnify` is PER ITEM and its centre is
// in that item's LOCAL frame. Put one on a picked item of a form that is
// several smooth-unioned pieces and it scales that piece's own field and leaves
// the others where they were — the surface pinches on one side of the blend and
// not the other. Nothing errors; it just comes out wrong, which is the worst
// shape a defect can have.
//
// `grab` had a resolver for that and `magnify` did not, so Pinch could not be a
// surface brush on a field at all (issue #391). This is that resolver.
//
// WHAT IS SHARED WITH MOVE, AND WHY. The half of a gesture that does not depend
// on its magnitude — which items the region reaches, through which of the
// layer's symmetry images, where each image's centre lands in the item's own
// scaled-local frame, what the radius becomes there — is not about dragging.
// It is about a BALL over a layer, and a magnify needs precisely the same
// answer. So `prepare_move` prepares this too and `PreparedMove` carries it;
// only the resolve half differs, and it differs by less than move's does:
//
//   grab     centre, radius, DISPLACEMENT   the displacement is a vector, so it
//                                           is rotated and scaled into the item
//   magnify  centre, radius, STRENGTH       the strength is a dimensionless
//                                           factor, so it crosses frames and
//                                           symmetry images untouched
//
// A reflection or a rotation of a radial scale is a radial scale of the same
// strength — both are isometries — so unlike a drag's displacement there is
// nothing per-image to compute.
//
// REACH IS THE BALL, both signs. Outside the radius the region weight is zero
// and the point is returned unchanged, so the field cannot differ there. A
// PINCH samples from outside the ball (its scale factor exceeds one) but is
// still only evaluated inside it, so the region a host must invalidate is the
// union of the images' balls with no dilation — where a drag's has to be
// dilated by the displacement.
//
// THE CHAIN RULE IS MOVE'S. A magnify goes at the FRONT of the node's chain for
// the same reason a grab does, and a gesture in progress REPLACES its own
// leading deformers rather than stacking a new one per frame. That rule lives
// in `moved_chain`, which matches a chain entry against the gesture by type,
// centre and radius, so it serves this gesture as it serves a drag. There is
// one rule and one place for it.
//
// WHAT IS NOT HERE. `pose` sits in the same sentence as grab and magnify in the
// deformer docs and has the same per-item gap, but it does not fit this shape:
// radial pose carries an AXIS, which is a direction and would have to be
// rotated per image, and pose-along-a-line has no finite support at all, so the
// reachability test this is built on does not apply to it. A resolver for pose
// is a separate piece of work with a different bound, not a parameter of this
// one.

#include <cstdint>
#include <vector>

#include "clay/brush/move.h"
#include "clay/scene/document.h"
#include "clay/scene/types.h"

namespace clay {
namespace brush {

struct MagnifySettings {
    // Radius of the region, in WORLD units.
    float radius = 0.3f;
    // Falloff curve across the region, from the shared easing table.
    std::uint8_t ease = 0;
    // No `front_only`. A radial scale has no direction to gate a half-space
    // on; grab's flag exists because a pull heads somewhere and this does not.
};

// The gesture's region, resolved against every item it reaches — the half that
// does not depend on the strength, so a live pinch pays for it once at pointer
// down and per frame pays only `resolve_prepared_magnify`.
//
// Identical to `prepare_move` with the same radius and ease, and implemented as
// a call to it: see the header note on what the two gestures share.
std::vector<PreparedMove> prepare_magnify(const scene::Layer& layer,
                                          kernel::cfloat3 world_centre,
                                          const MagnifySettings& settings = {},
                                          MovePrepareStats* out_stats = nullptr);

// The other half: the warp for a TOTAL strength — one magnify per image that
// reaches the item, ordered by value, and the rest of the gesture's identity in
// `gesture`. O(images), with no scene access.
//
// TOTAL, from the start of the gesture, never an increment on the last frame.
// A chain of increments composes scales that were each authored against a
// different intermediate surface, and the product is not the pinch the artist
// made — the same reason a drag takes its total displacement.
MoveWarp resolve_prepared_magnify(const PreparedMove& prepared, float strength);

// The same, into a warp the caller keeps from frame to frame, so a live gesture
// does not pay the allocator for the vectors every frame. `out` is cleared.
void resolve_prepared_magnify(const PreparedMove& prepared, float strength, MoveWarp* out);

// Resolve a world-space magnify into the warps that reproduce it on the layer's
// assembled surface.
//
// PURE: the layer is read, never written, so a host can preview the gesture
// before committing it. The warps are returned rather than applied because the
// caller decides which commands carry them, and one SetDeformersCmd per node
// inside an undo group is what makes a whole gesture one undo step.
//
// A POSITIVE strength swells the surface away from the centre (Magnify) and a
// NEGATIVE one gathers it toward (Pinch). One signed parameter, because they
// are one deformation — `scene::Deformer::magnify` says the same.
//
// Nothing comes back for a non-positive radius, a strength of zero, or a layer
// with no edit list: none of those describes a gesture, and a chain of no-op
// deformers is worse than none.
std::vector<MoveWarp> magnify_brush(const scene::Layer& layer, kernel::cfloat3 world_centre,
                                    float strength, const MagnifySettings& settings = {});

}  // namespace brush
}  // namespace clay
