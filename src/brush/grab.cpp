#include "clay/brush/grab.h"

#include "clay/scene/bounds.h"

namespace clay {
namespace brush {

namespace {

// Does the drag's sphere reach this item at all? The influence bound already
// covers the item's geometry plus its rounding and blend support, so an item
// whose bound misses the sphere cannot affect the field anywhere the warp is
// not the identity.
bool reaches(const math::Aabb& bound, kernel::cfloat3 centre, float radius) {
    if (bound.empty()) return false;
    if (bound.is_infinite()) return true;
    // Distance from the centre to the box, which is zero when it is inside.
    kernel::cfloat3 d = kernel::cf3(
        kernel::cmax(kernel::cmax(bound.min.x - centre.x, 0.0f), centre.x - bound.max.x),
        kernel::cmax(kernel::cmax(bound.min.y - centre.y, 0.0f), centre.y - bound.max.y),
        kernel::cmax(kernel::cmax(bound.min.z - centre.z, 0.0f), centre.z - bound.max.z));
    return kernel::clength(d) <= radius;
}

// Is this the trailing deformer of the drag we are continuing? Centre and
// radius are fixed for the life of one drag and only the displacement grows,
// so they identify it without a drag id having to be threaded through.
bool same_drag(const scene::Deformer& d, const scene::Deformer& fresh) {
    return d.type == kernel::cdeform_grab && d.k == fresh.k && d.a == fresh.a &&
           d.b == fresh.b && d.c == fresh.c;
}

void collect(const scene::SdfContent& content, const std::vector<scene::NodeId>& ids,
             const scene::Layer& layer, const GrabSettings& s,
             std::vector<GrabTarget>& out) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n || !n->visible) continue;
        if (n->is_group) {
            // A group carries an op and a blend, never a transform, so its
            // children are placed by the layer and their own transforms alone.
            collect(content, n->children, layer, s, out);
            continue;
        }
        if (!reaches(scene::item_influence_bound(*n, layer), s.centre, s.radius)) continue;

        scene::Deformer fresh = grab_local(layer.xform * n->xform, s);
        GrabTarget t{layer.id, id, n->deformers};
        if (!t.deformers.empty() && same_drag(t.deformers.back(), fresh))
            t.deformers.back() = fresh;
        else
            t.deformers.push_back(fresh);
        out.push_back(std::move(t));
    }
}

}  // namespace

scene::Deformer grab_local(const math::Transform& item_world, const GrabSettings& s) {
    const float scale = item_world.scale != 0.0f ? item_world.scale : 1.0f;
    // A point maps through the whole inverse transform; a displacement is a
    // VECTOR, so it takes the rotation and the scale but not the translation.
    // Getting that wrong is the bug this function exists to prevent.
    return scene::Deformer::grab(item_world.apply_inverse(s.centre), s.radius / scale,
                                 item_world.rotation.conjugate().rotate(s.displacement) / scale,
                                 s.ease, s.front_only);
}

std::vector<GrabTarget> grab_document(const scene::Document& doc, const GrabSettings& s) {
    std::vector<GrabTarget> out;
    if (!(s.radius > 0.0f)) return out;  // a drag with no reach touches nothing
    for (const scene::Layer& layer : doc.layers) {
        if (!layer.sdf || !layer.visible || layer.protected_from_edits()) continue;
        collect(*layer.sdf, layer.sdf->roots, layer, s, out);
    }
    return out;
}

}  // namespace brush
}  // namespace clay
