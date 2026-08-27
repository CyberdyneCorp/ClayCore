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
#include "clay/scene/bounds.h"
#include "clay/scene/document.h"

namespace clay {
namespace scene {

class CullPlan;

class CullIndex {
  public:
    explicit CullIndex(const Document& doc);

    const Document* document() const { return doc_; }

    // The total pad a cull needs beyond the caller's region, over the
    // document's visible SDF layers — what compile_document would otherwise
    // recompute per compile.
    //
    // Two terms, and both exist because an item can steer a value from outside
    // its own bound: a feathered replace's crossfade (feather_cull_pad) and a
    // smooth-union chain's running accumulation (blend_cull_pad). Named for
    // what it is rather than for the first of them, since it stopped being
    // only the feather.
    float cull_pad() const { return pad_; }

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

    // -- extending an index after an append --------------------------------
    //
    // A stroke appends one item per stamp, and rebuilding the index to add it
    // walks every node in the document recomputing bounds that did not move:
    // 2.45 ms at 50,000 items, of which 2.29 ms is bounds and 0.15 ms is the
    // pad. The same disease `compile_document_append` cures for the tape.
    //
    // Extends this index IN PLACE for a document that has gained `appended` at
    // the tail of its last visible SDF layer's roots, and nothing else.
    // Returns false and leaves the index UNTOUCHED when it cannot be sure that
    // is what happened -- no such layer, no chain for its roots, or `appended`
    // not actually at the tail in order. A refused caller rebuilds, which is
    // what it would have done anyway; a wrong extension is silent.
    //
    // The result is the index a fresh build would produce: the same chains
    // with the same cached bounds and the same prunability, and the same pad.
    // Chain ORDER may differ -- a fresh build emits a group's children before
    // the group's own chain and this appends them after -- which changes
    // nothing, because a plan keys chains by (layer, child list) and those
    // keys are unique. `test_cull_index.cpp` holds the tapes byte-identical
    // either way, which is the property that matters.
    //
    // WHY THE CACHED POINTERS SURVIVE, which the class contract otherwise
    // forbids relying on: entries hold `const Node*` into the layer's node
    // map, and that map is a `std::unordered_map`, whose references stay valid
    // across an insert even when it rehashes. A chain holds `&roots`, the
    // address of the vector rather than of its buffer, so growing it is fine
    // too. Those two facts are what make an append the ONE document mutation
    // this may be carried across.
    bool append(const std::vector<NodeId>& appended);

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

    // One layer's cull pad, kept as its TWO TERMS rather than as the sum, so
    // that an append can raise them from the subtree it added instead of
    // walking the layer's whole node map again. That walk was ALL BUT 0.0003 ms
    // of the 0.054 ms an append cost at 20,000 items -- a cheap walk against the
    // 2.45 ms rebuild it replaced, and essentially the whole of an append once
    // that rebuild was gone (#347).
    //
    // PER LAYER, which is the whole of why keeping the terms apart is exact:
    // the document's pad is a MAXIMUM OF SUMS over layers, so the terms fold
    // per layer and the maximum is taken of their sums. One global pair would
    // give a SUM OF MAXIMA -- larger, so safe, but no longer the number a fresh
    // build reports, and an appended index is held EQUAL to a rebuilt one.
    //
    // `nodes` is the size of the content's node map when the terms were taken.
    // The terms are raised from the appended subtree, so anything ELSE the map
    // gained would leave them low -- and low is the unsafe direction, a pad
    // that plans against too small a region. Checked rather than trusted: a map
    // that did not grow by exactly the subtree refuses the append, which costs
    // a rebuild.
    struct LayerPad {
        const Layer* layer;
        CullPadTerms terms;
        std::size_t nodes;
    };

    // What one appended subtree adds to a layer, gathered in one walk BEFORE
    // anything is written so a refusal can leave the index untouched.
    struct PadGain {
        std::size_t slot;  // index into pads_
        CullPadTerms terms;
        std::size_t nodes;  // the map size these terms are now good for
    };

    void build_chain(const SdfContent& content, const std::vector<NodeId>& ids,
                     const Layer& layer);
    // The chains built over `ids`, one per layer that compiles that list.
    std::vector<std::size_t> chains_over(const std::vector<NodeId>& ids) const;
    // False when the pad of any touched layer cannot be raised incrementally.
    bool plan_pads(const std::vector<std::size_t>& targets,
                   const std::vector<NodeId>& appended, std::vector<PadGain>* gains) const;
    // pad_ from pads_: the maximum of the layers' sums.
    void refresh_pad();

    const Document* doc_;
    float pad_ = 0.0f;
    std::vector<Chain> chains_;
    std::vector<LayerPad> pads_;
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
