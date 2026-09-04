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
// Layers combine with a hard union, so no cross-layer term has to be re-solved
// when one layer moves. That is the whole argument, and it is why the gate for
// this is a field EQUALITY test rather than a tolerance.
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
#include "clay/math/transform.h"
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
bool layer_scales_cleanly(const Layer& layer);

// How this layer moves to a proposed placement -- `placement_change` with the
// blend caveat above applied, which is the form the C ABI reports and the form
// a host should act on.
PlacementChange layer_placement_change(const Layer& layer, const math::Transform& to,
                                       kernel::cfloat3 to_axes);

}  // namespace clay::scene
