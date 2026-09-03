#include "clay/mesh/chunk_tree.h"

#include <algorithm>
#include <limits>

namespace clay {
namespace mesh {

void ChunkTree::clear() {
    boxes_.clear();
    ids_.clear();
    slot_.clear();
    order_.clear();
    nodes_.clear();
    refits_ = 0;
}

void ChunkTree::build(const ChunkTable& table) {
    clear();
    for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
        const SurfaceChunk* c = table.chunk(i);
        if (c == nullptr) continue;
        if (slot_.size() <= i) slot_.resize(i + 1, kNoNode);
        slot_[i] = static_cast<std::uint32_t>(boxes_.size());
        boxes_.push_back(c->bounds);
        ids_.push_back(i);
    }
    order_.resize(ids_.size());
    for (std::size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<std::uint32_t>(i);
    if (!order_.empty()) split(0, order_.size());
}

std::uint32_t ChunkTree::split(std::size_t begin, std::size_t end) {
    const std::uint32_t self = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(Node{});
    math::Aabb bounds;
    for (std::size_t i = begin; i < end; ++i) bounds.expand(boxes_[order_[i]]);
    nodes_[self].bounds = bounds;
    nodes_[self].begin = static_cast<std::uint32_t>(begin);
    nodes_[self].end = static_cast<std::uint32_t>(end);
    if (end - begin <= 4) return self;
    const kernel::cfloat3 ext = bounds.extent();
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    const std::size_t mid = begin + (end - begin) / 2;
    // The id tiebreak is what keeps the partition — and therefore the ORDER a
    // query returns chunks in — independent of the input permutation. A brush
    // sums a weighted normal over what it gathers and float addition does not
    // associate, so a query order that varied would make a stamp depend on the
    // tree's history.
    std::nth_element(order_.begin() + static_cast<std::ptrdiff_t>(begin),
                     order_.begin() + static_cast<std::ptrdiff_t>(mid),
                     order_.begin() + static_cast<std::ptrdiff_t>(end),
                     [&](std::uint32_t a, std::uint32_t b) {
                         const kernel::cfloat3 ca = boxes_[a].center(), cb = boxes_[b].center();
                         const float va = axis == 0 ? ca.x : (axis == 1 ? ca.y : ca.z);
                         const float vb = axis == 0 ? cb.x : (axis == 1 ? cb.y : cb.z);
                         if (va != vb) return va < vb;
                         return a < b;
                     });
    const std::uint32_t l = split(begin, mid);
    const std::uint32_t r = split(mid, end);
    nodes_[self].left = l;
    nodes_[self].right = r;
    return self;
}

void ChunkTree::query(const math::Aabb& box, std::vector<std::uint32_t>* out) const {
    out->clear();
    if (nodes_.empty()) return;
    descend(0, box, out);
}

void ChunkTree::descend(std::uint32_t node, const math::Aabb& box,
                        std::vector<std::uint32_t>* out) const {
    const Node& n = nodes_[node];
    if (!n.bounds.intersects(box)) return;
    if (n.left == kNoNode) {
        for (std::uint32_t i = n.begin; i < n.end; ++i)
            if (boxes_[order_[i]].intersects(box)) out->push_back(ids_[order_[i]]);
        return;
    }
    descend(n.left, box, out);
    descend(n.right, box, out);
}

std::uint32_t ChunkTree::nearest_chunk(kernel::cfloat3 p, float* out_distance) const {
    if (nodes_.empty()) {
        if (out_distance) *out_distance = 0.0f;
        return ChunkTable::kNoChunk;
    }
    float best = std::numeric_limits<float>::max();
    std::uint32_t best_id = ChunkTable::kNoChunk;
    descend_nearest(0, p, &best, &best_id);
    if (out_distance) *out_distance = best_id == ChunkTable::kNoChunk ? 0.0f : best;
    return best_id;
}

void ChunkTree::descend_nearest(std::uint32_t node, kernel::cfloat3 p, float* best,
                                std::uint32_t* best_id) const {
    const Node& n = nodes_[node];
    // Prune on the box distance, which is a lower bound on anything inside it.
    if (n.bounds.empty() || n.bounds.distance(p) > *best) return;
    if (n.left == kNoNode) {
        for (std::uint32_t i = n.begin; i < n.end; ++i) {
            const math::Aabb& b = boxes_[order_[i]];
            if (b.empty()) continue;
            const float d = b.distance(p);
            // The id tiebreak keeps the answer independent of the partition's
            // permutation, the same rule `split` follows.
            if (d < *best || (d == *best && ids_[order_[i]] < *best_id)) {
                *best = d;
                *best_id = ids_[order_[i]];
            }
        }
        return;
    }
    // Nearer child first, so the bound is tight before the far one is tested.
    const std::uint32_t l = n.left, r = n.right;
    const float dl = nodes_[l].bounds.empty() ? std::numeric_limits<float>::max()
                                              : nodes_[l].bounds.distance(p);
    const float dr = nodes_[r].bounds.empty() ? std::numeric_limits<float>::max()
                                              : nodes_[r].bounds.distance(p);
    if (dl <= dr) {
        descend_nearest(l, p, best, best_id);
        descend_nearest(r, p, best, best_id);
    } else {
        descend_nearest(r, p, best, best_id);
        descend_nearest(l, p, best, best_id);
    }
}

void ChunkTree::refit(const ChunkTable& table, const std::uint32_t* changed, std::size_t count) {
    if (nodes_.empty() || count == 0) return;
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t id = changed[i];
        if (id >= slot_.size() || slot_[id] == kNoNode) continue;
        const SurfaceChunk* c = table.chunk(id);
        if (c == nullptr) continue;
        boxes_[slot_[id]] = c->bounds;
        any = true;
    }
    if (!any) return;
    // The interior repair is a full bottom-up pass rather than a walk up from
    // each leaf. It is O(nodes) once per stamp against O(depth) per changed
    // chunk, and a stamp changes the chunks under the brush — which for the
    // footprints this exists to serve is enough of them that the per-leaf walks
    // re-visit the same ancestors repeatedly. Nodes are chunks/2, not vertices.
    refit_node(0);
    ++refits_;
}

void ChunkTree::refit_node(std::uint32_t node) {
    Node& n = nodes_[node];
    if (n.left == kNoNode) {
        math::Aabb bounds;
        for (std::uint32_t i = n.begin; i < n.end; ++i) bounds.expand(boxes_[order_[i]]);
        n.bounds = bounds;
        return;
    }
    refit_node(n.left);
    refit_node(n.right);
    math::Aabb bounds;
    bounds.expand(nodes_[n.left].bounds);
    bounds.expand(nodes_[n.right].bounds);
    nodes_[node].bounds = bounds;
}

}  // namespace mesh
}  // namespace clay
