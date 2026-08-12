#pragma once

// Per-revision cull acceleration (scene-model spec: per-brick tape culling).
//
// A per-brick culled compile consults, for every document node, bounds that
// are expensive to compute — item_geometry_bound re-tessellates a spline
// stroke, node_influence_bound recurses a group — and feather_cull_pad walks
// the whole document again. A 48-brick dab paid all of that 48 times per
// stamp, so the cost of one dab grew with everything already sculpted.
//
// The CullIndex computes each of those values ONCE per document revision. A
// CullPlan then serves a batch of regions (a dab's bricks, a re-mesh's
// vertex groups): one coarse cull of every chain against the batch's union
// region yields per-chain survivor lists carrying the cached bounds, so each
// per-brick compile walks only the items near the batch — no hashing, no
// bound recomputation — instead of the whole document. Both are pure
// accelerations: the compiler makes exactly the cull decisions it made
// without them, so the emitted tapes are byte-identical (regression-tested
// in tests/unit/test_cull_index.cpp).
//
// Coarse pruning is safe because a chain's per-brick region is contained in
// the batch union: an item whose bound misses the union misses every brick,
// so dropping it up front removes work, never an item a brick needed. Items
// whose influence is non-local (intersect, spatial morphs, infinite repeats,
// unbounded primitives — item_influence_is_local) always survive. Chains
// holding a feathered volume replace are never pruned: the compiler's choice
// between the feathered and the hard replace reads whether the cull dropped
// anything from THAT chain, so those chains keep the full walk to make the
// identical brick-by-brick choice.
//
// Lifetime: the index borrows the document (layer addresses, child-list
// addresses, node pointers). It is valid only until the document is next
// mutated — the same rule the cached whole-document tape lives by — and the
// C ABI keys it on the same revision counter. A plan borrows the index's
// document the same way and additionally must not outlive its index's
// revision.

#include <unordered_map>
#include <vector>

#include "clay/math/geom.h"
#include "clay/scene/document.h"

namespace clay {
namespace scene {

class CullPlan;

class CullIndex {
  public:
    explicit CullIndex(const Document& doc);

    const Document* document() const { return doc_; }

    // Max feather_cull_pad over the document's visible SDF layers — what
    // compile_document would otherwise recompute per compile.
    float feather_pad() const { return pad_; }

    // One coarse-cull survivor of a chain, in chain order: the node, and the
    // bound the compiler would have computed for it (item_geometry_bound for
    // items, node_influence_bound for groups) — bit-equal by construction,
    // it IS that function's result, computed once at index build.
    struct Entry {
        const Node* node;
        NodeId id;
        math::Aabb bound;
        bool local;  // false: never culled (item_influence_is_local)
    };

    // One coarse cull for a batch of compiles. `region` MUST contain every
    // cull region the plan is later used with (dilate each by its band
    // first, exactly as the per-brick CullRegion is built); the plan itself
    // applies the feather pad, as the per-brick test does.
    CullPlan plan(const math::Aabb& region) const;

  private:
    friend class CullPlan;

    // A (layer, address) pair. Chains depend on the layer as well as the
    // child list — an instanced layer shares SdfContent with its source, so
    // the shared roots vector is one chain per layer that compiles it.
    struct Key {
        const void* layer;
        const void* ids;
        bool operator==(const Key& o) const { return layer == o.layer && ids == o.ids; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::size_t h = reinterpret_cast<std::size_t>(k.layer);
            // splitmix-style scramble of the pair
            h ^= reinterpret_cast<std::size_t>(k.ids) + 0x9e3779b97f4a7c15ull + (h << 6) +
                 (h >> 2);
            return h;
        }
    };
    struct Chain {
        const Layer* layer;
        const std::vector<NodeId>* ids;
        std::vector<Entry> entries;  // visible children only, chain order
        bool prunable;               // no feathered volume replace among the items
    };

    void build_chain(const SdfContent& content, const std::vector<NodeId>& ids,
                     const Layer& layer);

    const Document* doc_;
    float pad_ = 0.0f;
    std::vector<Chain> chains_;
};

// The survivors of one coarse cull (CullIndex::plan). Handed to
// compile_document alongside the index and a per-brick CullRegion contained
// in the plan's region.
class CullPlan {
  public:
    // The surviving entries for a chain, or nullptr when the chain must
    // take the compiler's own uncached walk (unknown chains, and chains a
    // feathered replace forbids pruning).
    const std::vector<CullIndex::Entry>* chain(const Layer& layer,
                                               const std::vector<NodeId>& ids) const {
        auto it = pruned_.find(CullIndex::Key{&layer, &ids});
        return it == pruned_.end() ? nullptr : &it->second;
    }

  private:
    friend class CullIndex;
    std::unordered_map<CullIndex::Key, std::vector<CullIndex::Entry>, CullIndex::KeyHash>
        pruned_;
};

}  // namespace scene
}  // namespace clay
