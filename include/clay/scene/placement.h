#pragma once

// How a layer's PLACEMENT changed, and what that guarantees about its field.
//
// Moving or rotating a whole layer -- the gizmo on an object -- changes no
// shape. A layer's transform reaches the compiled tape in exactly three places
// (`src/scene/tape_build.cpp`): inside each item's inverse matrix, where it is a
// change of frame; as `rounding * layer.xform.scale` on items and groups; and
// through the scale the cull gate uses. For a placement that is rigid, all three
// are either a pure re-framing or unchanged, so the layer's surface afterwards
// is its surface beforehand moved by the same matrix. For one that adds a
// uniform scale, the second and third scale by the same factor the distances do,
// which is what keeps a rounded or blended shape SIMILAR to itself rather than
// merely relocated.
//
// LAYERS NO LONGER ALWAYS HARD-UNION, and this paragraph used to say they do.
// Since ABI 0.86.0 a visible SDF layer folds into the layers beneath it under
// its OWN composition (`LayerComposition`), which can carry a blend radius and a
// rounding -- the cross-layer terms this paragraph was written to say did not
// exist. What survives of the argument, and it is still what makes the gate a
// field EQUALITY test rather than a tolerance:
//
//   * The classification is about THE LAYER'S OWN FIELD -- "its surface
//     afterwards is its surface beforehand moved by the same matrix" -- and no
//     fold enters that. It was never a claim about the document's field, which
//     a min already broke: min(A, moved B) is not moved(min(A, B)).
//   * The fold's ROUNDING follows the layer's scale (`fold_layer` takes
//     `composition.rounding * layer_distance_scale(layer)`), exactly as an
//     item's does; its blend RADIUS does not, and that is why
//     `layer_scales_cleanly` reads the composition and returns false for any
//     positive `composition.blend.k`. See the paragraph on it below.
//   * What the fold DOES change is the INVALIDATION, not the classification: a
//     layer's edits reach further than the layer once something above folds
//     smoothly over it, and that widening is `scene::layer_reach_in_document`'s
//     job, not this file's. A Rigid or Similarity verdict says what the layer's
//     field did; it never said how little to redraw.
//
// A NON-UNIFORM layer scale is excluded on purpose. It changes the field's
// Lipschitz behaviour -- `cfi_scale_nonuniform` already records that for items
// -- so it classifies as General and nothing here is claimed about it.
//
// AND SO IS A SCALE CHANGE ON A LAYER THAT BLENDS, which is not what the plan
// for this said and was found by measuring rather than by reading. A layer's
// uniform scale multiplies an item's ROUNDING (`n->rounding *
// placed_distance_scale(...)`) and does NOT multiply its BLEND RADIUS
// (`emit_combine` pushes `blend.k` as it stands). So a layer of smooth-unioned
// items scaled by 2 keeps its old blend width against doubled shapes, and its
// field is not the old field times 2: measured on two boxes blended at k=0.12,
// the ratio came out 1.289 where the claim says 2.
//
// That inconsistency between rounding and blend is a defect in its own right
// and fixing it would change what every existing document with a scaled,
// blended layer evaluates to -- a separate decision, not a side effect of this
// one. Until it is taken, a scale change on such a layer is General, so nothing
// is promised that is not true.

#include <cstdint>

#include "clay/kernel/shim.h"
#include "clay/math/geom.h"
#include "clay/math/transform.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"

namespace clay::scene {

enum class PlacementKind : std::uint8_t {
    // Rotation and translation. Distances are unchanged.
    Rigid,
    // Those plus a uniform positive scale. Distances scale by `scale`.
    Similarity,
    // Anything else -- today only a non-uniform layer scale. Nothing is
    // guaranteed, and the caller takes the ordinary invalidation.
    General,
};

struct PlacementChange {
    PlacementKind kind = PlacementKind::General;
    // The uniform factor from the old placement to the new one; 1 for Rigid.
    // Meaningless for General, and left at 1 rather than at something a caller
    // might multiply by.
    float scale = 1.0f;
    // The world matrix taking the OLD placement to the NEW one. Applied to the
    // old surface it gives the new surface exactly, for Rigid and Similarity.
    // Identity for General, for the same reason `scale` is 1 there.
    math::cfloat4x4 delta = math::cfloat4x4{kernel::cf4(1, 0, 0, 0), kernel::cf4(0, 1, 0, 0),
                                            kernel::cf4(0, 0, 1, 0), kernel::cf4(0, 0, 0, 1)};
};

// Whether a per-axis scale is the identity, which is what makes a placement
// expressible as a similarity at all.
bool placement_scale_is_uniform(kernel::cfloat3 scale_axes);

// How the layer moves from one placement to another.
//
// Classified on the CHANGE and not on either placement alone: a layer already
// carrying a uniform scale of 2 that goes to 3 has moved by a similarity of
// 1.5, and asking whether the placement "is" a similarity would answer about
// the wrong thing. A non-uniform scale on EITHER side makes the change General,
// because the field on that side is not similar to the field on the other.
PlacementChange placement_change(const math::Transform& from, kernel::cfloat3 from_axes,
                                 const math::Transform& to, kernel::cfloat3 to_axes);

// Whether every distance term in the layer scales with the layer.
//
// True when no visible item or group carries a smooth blend, since the blend
// radius is the one term the layer's scale does not reach. A layer that scales
// cleanly may be classified as a Similarity; one that does not may still be
// classified Rigid, because a rigid change scales nothing and so cannot expose
// the difference.
//
// AND THE LAYER'S OWN COMPOSITION, which is a radius one level up: false for
// ANY positive `composition.blend.k`, whatever the op and whatever the profile.
// A radius is a radius -- a soft profile spends it as the blend radius, an
// extended mode (groove, shell, incise, pipe, the reliefs) as its own radius or
// depth with the profile ignored, and a PAINT as its colour falloff with the
// profile ignored too -- and `chain_blend_support`, which the document's cull
// pad is built on, already reads that one field as a world distance for every
// op. Enumerating the ops that spend it is the list this got wrong twice.
// Conservative for a hard Add, Subtract or Intersect, whose `k` the kernel
// ignores: they report General for a value that is doing nothing, which costs a
// recomputation rather than a picture that lags its own field.
// The fold's ROUNDING is not a term -- it is scaled with the layer, where the
// radius is not.
bool layer_scales_cleanly(const Layer& layer);

// How this layer moves to a proposed placement -- `placement_change` with the
// blend caveat above applied, which is the form the C ABI reports and the form
// a host should act on.
PlacementChange layer_placement_change(const Layer& layer, const math::Transform& to,
                                       kernel::cfloat3 to_axes);

// -- placements computed from a layer's own content --------------------------
//
// "Drop this subtool on the floor", "centre it", "put it back at the origin":
// three menu items every sculpting host carries, and three placements a host
// cannot compose correctly out of the layer transform pair. The single-factor
// reader REFUSES a layer carrying three different per-axis factors and the
// single-factor setter CLEARS them, so the obvious read-modify-write is either
// impossible or silently unsquashes the subtool. The correct composition is the
// per-axis pair, and this is that pair written once, on the inside.
//
// WHY THE ARITHMETIC IS THREE LINES AND HAS NO CASE ANALYSIS. A layer's world
// map is `layer.xform.matrix() * scale_matrix(layer.scale_axes)`, with the
// per-axis scale INNERMOST and `xform.position` applied last, so adding a
// world-space delta to `position` translates the layer's world content by
// exactly that delta -- whatever rotation and whatever squash the layer
// carries. No frame conversion, no branch on squashed vs unsquashed, and the
// same write serves all three rules.
//
// THESE TAKE A BOX RATHER THAN READING ONE. Which box is the caller's decision
// and it differs by binding: the C ABI composes the SDF, voxel and mesh arms
// (`layer_world_bounds`), while `pick::layer_bounds` answers the SDF arm alone
// -- and `scene` may not include `pick` or `voxel` by the layering rule anyway.
// What must NOT differ between bindings is which placement fields a computed
// placement writes, which is what `translated_layer_command` is: one policy,
// stated once, so the three rules and the two bindings cannot drift.

// Where the low face of `content` has to move to sit at `ground_y`. Y alone;
// the other two components are 0, so the box does not slide sideways.
kernel::cfloat3 ground_snap_delta(const math::Aabb& content, float ground_y);

// Where the centre of `content` has to move to sit at the world origin.
//
// The centre of the BOX, which is not a centre of mass: this engine holds no
// density, and a hollow shell and a solid of the same extent centre
// identically. An occupancy-weighted centroid would need a cell size named
// before it had an answer at all, and the answer would then move when an
// artist changed the layer's voxel size, which changes nothing about where the
// shape is.
kernel::cfloat3 origin_centre_delta(const math::Aabb& content);

// Where the layer has to move for its placement's TRANSLATION to be zero.
// Reads no content, which is why it is the one rule an empty layer can take.
kernel::cfloat3 origin_translation_delta(const Layer& layer);

// The ONE write a computed placement makes: `world_delta` added to
// `xform.position` and nothing else touched.
//
// The rotation, the uniform factor and the per-axis triple are carried through
// BIT FOR BIT, which is why this takes the Layer rather than composing the C
// reader and setter as the change's design.md first proposed. That round trip
// is not bit-exact in either half: the reader hands back an axis and an angle
// through `atan2` and the setter rebuilds a quaternion through `sin`/`cos`, and
// the per-axis reader answers the PRODUCT of the two scales, so the setter
// would fold `xform.scale` into `scale_axes` and change what the layer's own
// record says while leaving the composed map alone.
//
// ONE command, because `SetLayerTransformCmd` already carries the transform and
// the per-axis scale together -- one undo step whose inverse is the previous
// placement, and one invalidation. It is returned rather than applied so the
// caller can refuse first and pay for no bound it will not use.
SetLayerTransformCmd translated_layer_command(const Layer& layer, kernel::cfloat3 world_delta);

}  // namespace clay::scene
