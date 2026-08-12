#include "clay/scene/cull_index.h"

#include "clay/scene/bounds.h"

namespace clay {
namespace scene {

CullIndex::CullIndex(const Document& doc) : doc_(&doc) {
    for (const Layer& layer : doc.layers) {
        if (!layer.visible || layer.kind != LayerKind::Sdf || !layer.sdf) continue;
        // feather_cull_pad walks the flat node map, exactly as the compiler
        // did per compile: it must count a feathered volume wherever it
        // sits, including under a group this build never descends into.
        pad_ = kernel::cmax(pad_, feather_cull_pad(*layer.sdf, layer));
        build_chain(*layer.sdf, layer.sdf->roots, layer);
    }
}

// One Chain per (layer, child list), mirroring the compiler's traversal:
// every group's children are their own chain — cull_dropped, which the
// feathered-replace choice reads, is scoped to a single compile_list call,
// so prunability is decided chain by chain rather than for the whole tree.
void CullIndex::build_chain(const SdfContent& content, const std::vector<NodeId>& ids,
                            const Layer& layer) {
    Chain chain;
    chain.layer = &layer;
    chain.ids = &ids;
    chain.prunable = true;
    chain.entries.reserve(ids.size());
    for (NodeId id : ids) {
        const Node* n = content.find(id);
        if (!n || !n->visible) continue;
        Entry e;
        e.node = n;
        e.id = id;
        if (n->is_group) {
            e.bound = node_influence_bound(content, id, layer);
            // A group is always cull-tested; its bound reports infinity for
            // a non-local subtree, which the survive test lets through.
            e.local = true;
            build_chain(content, n->children, layer);
        } else {
            e.bound = item_geometry_bound(*n, layer);
            e.local = item_influence_is_local(*n);
            // The compiler's choice between the feathered and the hard
            // replace depends on whether the cull dropped ANYTHING from this
            // chain — including items a coarse cull would hide from it — so
            // such a chain keeps the full walk and decides per brick.
            if (item_is_feathered_replace(*n)) chain.prunable = false;
        }
        chain.entries.push_back(e);
    }
    chains_.push_back(std::move(chain));
}

CullPlan CullIndex::plan(const math::Aabb& region) const {
    CullPlan plan;
    plan.pruned_.reserve(chains_.size());
    // Dilated by the feather pad exactly as the per-brick test is
    // (Compiler::begin_cull), so coarse survival stays a superset of
    // per-brick survival.
    const math::Aabb test = pad_ > 0.0f ? region.dilated(pad_) : region;
    for (const Chain& chain : chains_) {
        if (!chain.prunable) continue;
        std::vector<Entry> kept;
        for (const Entry& e : chain.entries)
            // The compiler's own test (Compiler::culled), inverted: an entry
            // survives unless it is local, finite and misses the region.
            if (!e.local || e.bound.is_infinite() || e.bound.intersects(test))
                kept.push_back(e);
        // Stored even when nothing was dropped: the survivors carry the
        // cached bounds, so a planned chain never recomputes one per brick.
        plan.pruned_.emplace(Key{chain.layer, chain.ids}, std::move(kept));
    }
    return plan;
}

}  // namespace scene
}  // namespace clay
