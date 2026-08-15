#include "clay/brush/lattice_gizmo.h"

#include <algorithm>

namespace clay {
namespace brush {

using kernel::cf3;
using kernel::cfloat3;

namespace {

int clamped(int n) {
    return std::max(2, std::min(scene::Deformer::kMaxLatticeDivisions, n));
}

}  // namespace

std::size_t GizmoCage::point_count() const {
    return static_cast<std::size_t>(clamped(nx)) * static_cast<std::size_t>(clamped(ny)) *
           static_cast<std::size_t>(clamped(nz));
}

cfloat3 GizmoCage::rest(int i, int j, int k) const {
    auto along = [](float lo, float hi, int idx, int count) {
        return count < 2 ? lo
                         : lo + (hi - lo) * (static_cast<float>(idx) /
                                             static_cast<float>(count - 1));
    };
    return cf3(along(box_min.x, box_max.x, i, clamped(nx)),
               along(box_min.y, box_max.y, j, clamped(ny)),
               along(box_min.z, box_max.z, k, clamped(nz)));
}

cfloat3 GizmoCage::world_rest(int i, int j, int k) const {
    return placement.apply(rest(i, j, k));
}

bool GizmoCage::is_identity() const {
    for (const cfloat3& o : offsets)
        if (o.x != 0.0f || o.y != 0.0f || o.z != 0.0f) return false;
    return true;
}

namespace {

void collect(const scene::SdfContent& content, const scene::Layer& layer,
             const std::vector<scene::NodeId>& ids, const GizmoCage& cage,
             std::vector<LatticeWarp>* out) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n || !n->visible) continue;
        if (n->is_group) {
            // A group takes no warp of its own: its transform does not reach
            // its children here, so the children are what carry the cage.
            collect(content, layer, n->children, cage, out);
            continue;
        }

        // NO REACHABILITY TEST, and that is the difference from `move_brush`
        // rather than an omission. A lattice's displacement outside its box is
        // CLAMPED, not zero, so an item out there travels rigidly with the
        // nearest part of the cage — which is what a gizmo acting on a whole
        // subtool means. Skipping "distant" items would silently tear the form.

        // The item's world frame. `layer.xform * node.xform` is the whole
        // story: the evaluator composes exactly these two, and a group in
        // between contributes nothing.
        const math::Transform world = layer.xform * n->xform;
        // Local -> cage: into the world by the item's frame, then out of the
        // world by the cage's. Rigid with uniform scale on both sides, so the
        // composition is too, and the deformer's bound needs no extra term.
        const math::Transform local_to_cage = cage.placement.inverse() * world;

        LatticeWarp warp;
        warp.node = id;
        warp.deformer = scene::Deformer::lattice_transformed(cage.box_min, cage.box_max,
                                                             local_to_cage, cage.nx, cage.ny,
                                                             cage.nz);
        // The offsets are the cage's, in the cage's own space, and every item
        // gets the same ones — the transform is what makes them mean the same
        // thing in each item's frame.
        const std::size_t n_points = std::min(warp.deformer.cage.size(), cage.offsets.size());
        for (std::size_t i = 0; i < n_points; ++i) warp.deformer.cage[i] = cage.offsets[i];
        out->push_back(warp);
    }
}

}  // namespace

std::vector<LatticeWarp> lattice_gizmo(const scene::Layer& layer, const GizmoCage& cage) {
    std::vector<LatticeWarp> out;
    if (!layer.sdf) return out;
    // An untouched cage warps nothing anywhere, and a chain of no-op deformers
    // is worse than none — each still costs a tape record on every evaluation.
    if (cage.is_identity()) return out;
    // A cage with no extent has no parameter to read on the collapsed axes and
    // nothing to span; the deformer would clamp every point to one control
    // point's neighbourhood, which is not what any caller means by it.
    if (!(cage.box_max.x > cage.box_min.x || cage.box_max.y > cage.box_min.y ||
          cage.box_max.z > cage.box_min.z))
        return out;
    collect(*layer.sdf, layer, layer.sdf->roots, cage, &out);
    return out;
}

std::vector<scene::Deformer> caged_chain(const scene::Node& node, const LatticeWarp& warp) {
    std::vector<scene::Deformer> chain;
    chain.reserve(node.deformers.size() + 1);
    chain.push_back(warp.deformer);
    // Any cage this resolver put there before is replaced rather than stacked,
    // so dragging a control point across frames does not accumulate a chain of
    // them. Anything else the item carries is kept, behind the cage.
    for (const scene::Deformer& d : node.deformers)
        if (d.type != kernel::cdeform_lattice_xform) chain.push_back(d);
    return chain;
}

}  // namespace brush
}  // namespace clay
