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
    const math::Aabb drag(centre - cf3(radius, radius, radius),
                          centre + cf3(radius, radius, radius));
    return !(influence.max.x < drag.min.x || influence.min.x > drag.max.x ||
             influence.max.y < drag.min.y || influence.min.y > drag.max.y ||
             influence.max.z < drag.min.z || influence.min.z > drag.max.z);
}

void collect(const scene::SdfContent& content, const scene::Layer& layer,
             const std::vector<scene::NodeId>& ids, cfloat3 world_centre,
             cfloat3 world_displacement, const MoveSettings& settings,
             std::vector<MoveWarp>* out) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n || !n->visible) continue;
        if (n->is_group) {
            // A group takes no warp of its own: its transform does not reach
            // its children here, so the children are what carry the drag.
            collect(content, layer, n->children, world_centre, world_displacement, settings,
                    out);
            continue;
        }
        if (!reaches(scene::item_influence_bound(*n, layer), world_centre, settings.radius))
            continue;

        // The item's world frame. `layer.xform * node.xform` is the whole
        // story: the evaluator composes exactly these two, and a group in
        // between contributes nothing.
        const math::Transform world = layer.xform * n->xform;
        const float scale = world.scale != 0.0f ? world.scale : 1.0f;

        // A point maps through the full inverse; a DISPLACEMENT is a vector, so
        // it takes the rotation and the scale but not the translation.
        const cfloat3 local_centre = world.apply_inverse(world_centre);
        const cfloat3 local_displacement =
            world.rotation.conjugate().rotate(world_displacement) / scale;
        const float local_radius = settings.radius / scale;

        MoveWarp warp;
        warp.node = id;
        warp.deformer = scene::Deformer::grab(local_centre, local_radius, local_displacement,
                                              settings.ease, settings.front_only);
        out->push_back(warp);
    }
}

// Is the chain's leading warp this same drag, one frame earlier?
bool continues_drag(const scene::Node& node, const scene::Deformer& fresh) {
    if (node.deformers.empty()) return false;
    const scene::Deformer& lead = node.deformers.front();
    return lead.type == kernel::cdeform_grab && lead.k == fresh.k && lead.a == fresh.a &&
           lead.b == fresh.b && lead.c == fresh.c;
}

}  // namespace

std::vector<MoveWarp> move_brush(const scene::Layer& layer, cfloat3 world_centre,
                                 cfloat3 world_displacement, const MoveSettings& settings) {
    std::vector<MoveWarp> out;
    if (!(settings.radius > 0.0f)) return out;
    if (kernel::clength(world_displacement) <= 0.0f) return out;
    if (!layer.sdf) return out;
    collect(*layer.sdf, layer, layer.sdf->roots, world_centre, world_displacement, settings,
            &out);
    return out;
}

std::vector<scene::Deformer> moved_chain(const scene::Node& node, const MoveWarp& warp) {
    // FRONT, not back. deformers[0] warps the point first and is therefore the
    // outermost warp on the geometry, which is what "drag the assembled shape"
    // means; appended, the grab's region weight would be read at a point the
    // existing deformers had already moved.
    std::vector<scene::Deformer> chain;
    chain.reserve(node.deformers.size() + 1);
    chain.push_back(warp.deformer);
    // ...and REPLACING the leading warp when it belongs to the drag still in
    // progress, rather than stacking another on top of it. A drag holds its
    // centre and radius fixed and only grows the displacement, so those two
    // identify it without a drag id having to be threaded through. Without
    // this a drag appends one deformer per frame: the chain grows without
    // bound, and the declared Lipschitz compounds with every frame of it.
    const std::size_t skip = continues_drag(node, warp.deformer) ? 1u : 0u;
    chain.insert(chain.end(), node.deformers.begin() + skip, node.deformers.end());
    return chain;
}

}  // namespace brush
}  // namespace clay
