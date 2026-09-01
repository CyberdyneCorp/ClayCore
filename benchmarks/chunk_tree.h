#pragma once

// A TOP-LEVEL TREE OVER A CHUNK TABLE'S BOUNDS, for the two benchmarks that
// need to ask "which chunks does this brush reach" the way a runtime asks it.
//
// SHARED RATHER THAN COPIED, and the reason is a bug both copies had. The first
// version of each timed the query as a walk over every chunk testing its
// bounds, which is O(chunks): `bench_surface_chunks` then reported the query
// getting four times FASTER every time the chunk size doubled, purely because
// there were half as many chunks to scan, and `bench_extreme_poly` reported the
// query stage growing with the MODEL at a fixed footprint — which is the exact
// claim that benchmark exists to test, failed by its own harness. A real query
// descends a tree.
//
// It is a median split over centroids, which is the same partition rule the
// chunk partitioners themselves use, so the tree's shape is not a third thing
// to reason about.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/surface_chunks.h"

namespace clay {
namespace bench {

class ChunkTree {
  public:
    void build(const mesh::ChunkTable& table) {
        boxes_.clear();
        ids_.clear();
        for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
            const mesh::SurfaceChunk* c = table.chunk(i);
            if (c == nullptr) continue;
            boxes_.push_back(c->bounds);
            ids_.push_back(i);
        }
        order_.resize(ids_.size());
        for (std::size_t i = 0; i < order_.size(); ++i)
            order_[i] = static_cast<std::uint32_t>(i);
        nodes_.clear();
        if (!order_.empty()) split(0, order_.size());
    }

    void query(const math::Aabb& box, std::vector<std::uint32_t>* out) const {
        out->clear();
        if (nodes_.empty()) return;
        descend(0, box, out);
    }

    std::size_t nodes() const { return nodes_.size(); }

  private:
    struct Node {
        math::Aabb bounds;
        std::uint32_t begin = 0, end = 0;
        std::uint32_t left = 0xffffffffu, right = 0xffffffffu;
    };

    std::uint32_t split(std::size_t begin, std::size_t end) {
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

    void descend(std::uint32_t node, const math::Aabb& box, std::vector<std::uint32_t>* out) const {
        const Node& n = nodes_[node];
        if (!n.bounds.intersects(box)) return;
        if (n.left == 0xffffffffu) {
            for (std::uint32_t i = n.begin; i < n.end; ++i)
                if (boxes_[order_[i]].intersects(box)) out->push_back(ids_[order_[i]]);
            return;
        }
        descend(n.left, box, out);
        descend(n.right, box, out);
    }

    std::vector<math::Aabb> boxes_;
    std::vector<std::uint32_t> ids_;
    std::vector<std::uint32_t> order_;
    std::vector<Node> nodes_;
};


}  // namespace bench
}  // namespace clay
