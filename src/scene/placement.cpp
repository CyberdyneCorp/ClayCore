#include "clay/scene/placement.h"

#include <cmath>

namespace clay::scene {

using kernel::cfloat3;
using math::cfloat4x4;

namespace {

// Exact, not near. A layer whose per-axis scale is 1.0000001 is a non-uniform
// scale and the guarantee does not hold for it; a tolerance here would be this
// file deciding how much wrongness a host may not be told about. The value gets
// to this point unmodified -- it is stored as the caller set it -- so an equality
// test is answerable rather than fragile.
bool is_one(float v) { return v == 1.0f; }

}  // namespace

bool placement_scale_is_uniform(cfloat3 scale_axes) {
    return is_one(scale_axes.x) && is_one(scale_axes.y) && is_one(scale_axes.z);
}

PlacementChange placement_change(const math::Transform& from, cfloat3 from_axes,
                                 const math::Transform& to, cfloat3 to_axes) {
    PlacementChange out;
    if (!placement_scale_is_uniform(from_axes) || !placement_scale_is_uniform(to_axes))
        return out;  // General, with the identity delta and a factor of 1

    // A scale that is zero or negative has no similarity to describe: the
    // former collapses the field and the latter reflects it, and neither is
    // what `scale` means on a Transform. Left as General rather than reported
    // as a similarity with a factor a caller would divide by.
    if (!(from.scale > 0.0f) || !(to.scale > 0.0f) || !std::isfinite(from.scale) ||
        !std::isfinite(to.scale))
        return out;

    out.scale = to.scale / from.scale;
    out.kind = (out.scale == 1.0f) ? PlacementKind::Rigid : PlacementKind::Similarity;
    // The matrix taking a point placed by `from` to where `to` places it.
    out.delta = math::mul(to.matrix(), from.inverse_matrix());
    return out;
}

namespace {

// Does the layer's fold carry a radius in world units that the layer's scale
// will not reach? ANY positive `blend.k` does, whatever the profile says and
// whatever the op is. A RADIUS IS A RADIUS.
//
// Neither the profile nor the op is the test, and enumerating either was how
// this went wrong twice. `blend.k` is the one field a composition has for a
// world-space distance, and it is spent as one three different ways:
//
//   * a SOFT profile spends it as the blend radius -- the item-level case one
//     level up;
//   * an EXTENDED mode (groove, shell, incise, pipe, the reliefs...) spends it
//     as its own radius, depth or amplitude and IGNORES the profile entirely
//     (scene/types.h says so at the enumerators);
//   * PAINT spends it as the colour falloff. `ctape_combine_values` fades over
//     `cmax(ctape_blend_support(profile, k), k)`, so a HARD-profile paint fades
//     over exactly k -- and a predicate that read the profile, then rescued the
//     extended range, classified that as a similarity while its colour reached
//     an absolute 0.3 across a layer being scaled.
//
// AND THE ENGINE HAS ALREADY COMMITTED TO READING IT THAT WAY.
// `chain_blend_support` -- the single definition of how far a combine reaches,
// which `layer_blend_support`, `folds_from_layer_support` and
// `document_cull_pad` are all built on -- is `cmax(blend.support(), blend.k)`,
// so a fold with k = 0.3 dilates the DOCUMENT'S CULL PAD by 0.3 for every op
// and every profile. A classifier answering "similarity" there would be a
// second answer to a question the pad has already answered, which is the exact
// shape of failure this change is organised against.
//
// WHERE THIS IS CONSERVATIVE, AND WHAT THAT COSTS. For a hard-profile Add,
// Subtract or Intersect the kernel ignores k (`ctape_smin` with a hard profile
// is a plain min, and the add's colour weight is its 0-or-1 select), so such a
// layer really is a similarity of its own field and this declines it the cheap
// placement path anyway. The cost is one recomputation on a scale gesture, for
// a value that is doing nothing; the other direction is a picture that lags its
// own field. Those are not symmetric, and a fourth enumeration of "which ops
// read k" is the thing a fifth one goes on to disagree with.
//
// The composition's ROUNDING is not here because it IS scaled -- `fold_layer`
// takes `comp.rounding * layer_distance_scale(layer)`, which
// test_layer_fold.cpp holds against the reference evaluator at a scale of 2 --
// so the asymmetry is genuinely the radius alone.
bool composition_radius_ignores_scale(const LayerComposition& c) { return c.blend.k > 0.0f; }

}  // namespace

bool layer_scales_cleanly(const Layer& layer) {
    if (layer.kind != LayerKind::Sdf || !layer.sdf) return true;  // nothing to scale wrongly
    // THE LAYER'S OWN FOLD FIRST, and for exactly the reason the items below
    // are checked: a fold radius is an absolute world distance and the layer's
    // scale does not reach it, so a layer whose items all scale cleanly but
    // whose COMPOSITION carries one is not a similarity of its own field.
    //
    // It matters more here than in the item case: this verdict feeds
    // clay_layer_placement_begin/_update/_commit, the gesture whose whole
    // purpose is to SKIP work, so a wrong Similarity is a picture that lags its
    // own field rather than a recomputation that costs a little.
    if (composition_radius_ignores_scale(layer.composition)) return false;
    for (const auto& [id, n] : layer.sdf->nodes()) {
        (void)id;
        if (!n.visible) continue;
        // The blend radius is the term the layer's scale does not reach. A hard
        // combine, or a smooth one with no radius, has nothing to be wrong
        // about.
        //
        // NOT `composition_radius_ignores_scale` above, and the difference is
        // deliberate rather than an oversight: an ITEM with a hard profile and
        // an extended op, a paint, or a positive `k` its op ignores carries the
        // same absolute radius and takes the cheap path here. That is behaviour
        // this change did not introduce and does not alter -- it predates layer
        // composition, it reclassifies documents that carry no fold at all, and
        // the v0.84.0 known limits already record the item-level asymmetry.
        // Widening it is its own change with its own measurement.
        if (n.blend.profile != BlendProfile::Hard && n.blend.k > 0.0f) return false;
    }
    return true;
}

PlacementChange layer_placement_change(const Layer& layer, const math::Transform& to,
                                       cfloat3 to_axes) {
    PlacementChange change = placement_change(layer.xform, layer.scale_axes, to, to_axes);
    // A scale the blend radius will not follow is not a similarity of this
    // layer's field, whatever it is of its shapes.
    if (change.kind == PlacementKind::Similarity && !layer_scales_cleanly(layer))
        return PlacementChange{};
    return change;
}

// -- placements computed from a layer's own content --------------------------

cfloat3 ground_snap_delta(const math::Aabb& content, float ground_y) {
    return kernel::cf3(0.0f, ground_y - content.min.y, 0.0f);
}

cfloat3 origin_centre_delta(const math::Aabb& content) {
    const cfloat3 c = content.center();
    return kernel::cf3(-c.x, -c.y, -c.z);
}

cfloat3 origin_translation_delta(const Layer& layer) {
    const cfloat3 p = layer.xform.position;
    // `x + (-x)` is exactly zero for every finite float, so the position this
    // lands on is the origin and not something a hair off it.
    return kernel::cf3(-p.x, -p.y, -p.z);
}

SetLayerTransformCmd translated_layer_command(const Layer& layer, cfloat3 world_delta) {
    SetLayerTransformCmd cmd{layer.id, layer.xform, layer.scale_axes};
    cmd.xform.position = layer.xform.position + world_delta;
    return cmd;
}

}  // namespace clay::scene
