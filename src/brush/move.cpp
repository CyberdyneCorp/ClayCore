// The Move brush (brush-engine spec, add-move-brush). See
// include/clay/brush/move.h for the three ways a caller gets this wrong and why
// the resolver owns them.

#include "clay/brush/move.h"

#include <cmath>

#include "clay/scene/bounds.h"

namespace clay {
namespace brush {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// Does the drag reach this item at all? Compared as boxes: the item's influence
// bound is one already, and a sphere's is exact enough for a cull whose only
// job is to not emit a deformer that provably does nothing.
bool reaches(const math::Aabb& influence, cfloat3 centre, float radius) {
    if (influence.empty()) return false;
    if (influence.is_infinite()) return true;  // a non-local op: never cull it
    const math::Aabb drag{centre - cf3(radius, radius, radius),
                          centre + cf3(radius, radius, radius)};
    return !(influence.max.x < drag.min.x || influence.min.x > drag.max.x ||
             influence.max.y < drag.min.y || influence.min.y > drag.max.y ||
             influence.max.z < drag.min.z || influence.min.z > drag.max.z);
}

// The items a drag reaches, and their frames. Everything here depends on the
// anchor and the radius and NOT on the displacement, which is what lets a live
// drag pay for it once — see brush/move.h.
void collect(const scene::SdfContent& content, const scene::Layer& layer,
             const std::vector<scene::NodeId>& ids, cfloat3 world_centre,
             const MoveSettings& settings, std::vector<PreparedMove>* out,
             MovePrepareStats* stats) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n) continue;
        if (stats) ++stats->visited;
        if (!n->visible) continue;
        if (n->is_group) {
            // A group takes no warp of its own: its transform does not reach
            // its children here, so the children are what carry the drag.
            collect(content, layer, n->children, world_centre, settings, out, stats);
            continue;
        }
        if (!reaches(scene::item_influence_bound(*n, layer), world_centre, settings.radius))
            continue;

        // The item's world frame. `layer.xform * node.xform` PLUS the item's
        // own per-axis scale, which is innermost and which this composition
        // used to drop (#320): the tape applies the whole inverse before the
        // deformer chain, so a warp authored in the placed frame lands
        // somewhere the squashed item is not. Dragging the surface of an item
        // scaled 3x on one axis did nothing at all.
        const math::Transform world = layer.xform * n->xform;
        const float scale = world.scale != 0.0f ? world.scale : 1.0f;
        const cfloat3 axes = n->scale_axes;

        PreparedMove prepared;
        prepared.node = id;
        // A point maps through the full inverse; a DISPLACEMENT is a vector, so
        // it takes the rotation and the scale but not the translation. The
        // per-axis scale comes off both of them last, because it is innermost —
        // the displacement half of that is `resolve_prepared_move`, held to the
        // same order and the same operations.
        prepared.local_centre =
            scene::into_scaled_local(world.apply_inverse(world_centre), axes);
        prepared.inverse_rotation = world.rotation.conjugate();
        prepared.world_scale = scale;
        prepared.scale_axes = axes;
        // A grab carries ONE radius, and a squashed frame turns the artist's
        // world-space sphere into a local ELLIPSOID, so no scalar is exact.
        // Dividing by the LARGEST factor is the conservative reading: every
        // world reach `R * s_i * scale` is then at most the radius circled, so
        // a drag never takes geometry the artist did not enclose. Under-reach
        // is recoverable by dragging again; over-reach is not. This is the same
        // instinct cscale_nu_dist follows by taking the smallest factor.
        prepared.local_radius = settings.radius / (scale * scene::scale_axes_reach(axes));
        prepared.ease = settings.ease;
        prepared.front_only = settings.front_only;
        if (stats) ++stats->reached;
        out->push_back(prepared);
    }
}

// Is the chain's leading warp this same drag, one frame earlier?
bool continues_drag(const std::vector<scene::Deformer>& chain, const scene::Deformer& fresh) {
    if (chain.empty()) return false;
    const scene::Deformer& lead = chain.front();
    return lead.type == kernel::cdeform_grab && lead.k == fresh.k && lead.a == fresh.a &&
           lead.b == fresh.b && lead.c == fresh.c;
}

}  // namespace

std::vector<PreparedMove> prepare_move(const scene::Layer& layer, cfloat3 world_centre,
                                       const MoveSettings& settings,
                                       MovePrepareStats* out_stats) {
    std::vector<PreparedMove> out;
    if (out_stats) *out_stats = MovePrepareStats{};
    if (!(settings.radius > 0.0f)) return out;
    if (!layer.sdf) return out;
    collect(*layer.sdf, layer, layer.sdf->roots, world_centre, settings, &out, out_stats);
    return out;
}

MoveWarp resolve_prepared_move(const PreparedMove& prepared, cfloat3 total_world_displacement) {
    const cfloat3 local_displacement = scene::into_scaled_local(
        prepared.inverse_rotation.rotate(total_world_displacement) / prepared.world_scale,
        prepared.scale_axes);
    MoveWarp warp;
    warp.node = prepared.node;
    warp.deformer =
        scene::Deformer::grab(prepared.local_centre, prepared.local_radius, local_displacement,
                              prepared.ease, prepared.front_only);
    return warp;
}

std::vector<MoveWarp> move_brush(const scene::Layer& layer, cfloat3 world_centre,
                                 cfloat3 world_displacement, const MoveSettings& settings) {
    std::vector<MoveWarp> out;
    if (kernel::clength(world_displacement) <= 0.0f) return out;
    // Prepare-then-resolve, so there is ONE resolver and a live drag cannot
    // drift away from what a commit through this entry point would produce.
    // Preparing per call is what this always did — the traversal is the same
    // one — and the guards it keeps are the ones only it can answer.
    const std::vector<PreparedMove> prepared = prepare_move(layer, world_centre, settings);
    out.reserve(prepared.size());
    for (const PreparedMove& p : prepared)
        out.push_back(resolve_prepared_move(p, world_displacement));
    return out;
}

std::vector<scene::Deformer> moved_chain(const scene::Node& node, const MoveWarp& warp) {
    return moved_chain(node.deformers, warp);
}

std::vector<scene::Deformer> moved_chain(const std::vector<scene::Deformer>& existing,
                                         const MoveWarp& warp) {
    // FRONT, not back. deformers[0] warps the point first and is therefore the
    // outermost warp on the geometry, which is what "drag the assembled shape"
    // means; appended, the grab's region weight would be read at a point the
    // existing deformers had already moved.
    std::vector<scene::Deformer> chain;
    chain.reserve(existing.size() + 1);
    chain.push_back(warp.deformer);
    // ...and REPLACING the leading warp when it belongs to the drag still in
    // progress, rather than stacking another on top of it. A drag holds its
    // centre and radius fixed and only grows the displacement, so those two
    // identify it without a drag id having to be threaded through. Without
    // this a drag appends one deformer per frame: the chain grows without
    // bound, and the declared Lipschitz compounds with every frame of it.
    const std::size_t skip = continues_drag(existing, warp.deformer) ? 1u : 0u;
    chain.insert(chain.end(), existing.begin() + static_cast<std::ptrdiff_t>(skip),
                 existing.end());
    return chain;
}

}  // namespace brush
}  // namespace clay
