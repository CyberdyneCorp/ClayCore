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

bool layer_scales_cleanly(const Layer& layer) {
    if (layer.kind != LayerKind::Sdf || !layer.sdf) return true;  // nothing to scale wrongly
    for (const auto& [id, n] : layer.sdf->nodes()) {
        (void)id;
        if (!n.visible) continue;
        // The blend radius is the term the layer's scale does not reach. A hard
        // combine, or a smooth one with no radius, has nothing to be wrong
        // about.
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

}  // namespace clay::scene
