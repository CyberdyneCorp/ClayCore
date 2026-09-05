#pragma once

// An ephemeral fp32 field cache for an old, stable EDIT-LIST PREFIX
// (sdf-prefix-cache spec), so that interaction costs the work being edited
// rather than everything the artist has already sculpted.
//
// -- the problem --------------------------------------------------------------
//
// A dirty region over worked geometry re-evaluates every item that contributes
// there, and almost none of them changed. Measured (#306) at a 0.05 voxel, one
// dab into 12 bricks: 0.23 ms at 200 items, 18.07 ms at 50,000. Consolidating
// the layer shows the floor — the same work fell to about a third — but a bake
// discards the parameters of everything it absorbs, so the cure costs the
// artist their history.
//
// -- what this is -------------------------------------------------------------
//
// The same split, WITHOUT the loss: sample the old roots into a volume and keep
// the nodes. The document is untouched, every item stays editable, and the
// cache is derived state that may be dropped at any moment.
//
//     roots [0, K)          roots [K, N)
//     ───────────────       ─────────────
//     cached FieldVolume  + live suffix    ==  the layer's field
//
// DELETING EVERY CACHE ENTRY MUST BE SEMANTICALLY EQUIVALENT TO FLUSHING A CPU
// CACHE: slower, and identical output. That is the rule every decision here is
// measured against, and the tests assert the output half directly against a
// full walk.
//
// -- why it can be exact ------------------------------------------------------
//
// The tape compiler already emits a layer's chain as a FOLD at item boundaries:
// after every root the stack holds exactly one value. `compile_layer_prefix`
// and `compile_layer_suffix` (scene/tape.h) name that boundary, and
// `eval::eval_points_seeded` continues from it. Measured over 20,000 random
// points, prefix-tape-then-seeded-suffix is BIT-IDENTICAL to compiling the
// whole document — not a tolerance, zero difference.
//
// So the only question this file has to answer is when the cached VOLUME is an
// acceptable stand-in for that seed.
//
// -- the far-bound rule, which is the whole correctness argument --------------
//
// A sparse FieldVolume has two regimes. Where it stores samples, `eval` is
// interpolation of them. Where it does not, `eval` is a conservative FAR BOUND
// — a number chosen so a marcher cannot overstep, and emphatically not the
// distance the history actually had there.
//
// Measured, seeding a suffix from a prefix volume and comparing against the
// full walk:
//
//     prefix volume HAS samples      worst error 3e-7   (float rounding)
//     prefix volume has NONE         worst error 0.27   (~14 cells)
//
// and the 14 cells barely move with cell size or blend width, so it is
// categorical rather than a tuning problem. A blend in the suffix folded onto a
// far bound is silently wrong by many cells.
//
// THEREFORE: the volume seeds a window only where it stores every sample of it,
// and anywhere else the prefix TAPE is evaluated instead. Correct either way —
// only the cost differs — and `fallback_windows` counts how often the slow
// answer was needed, because a cache whose fallback rate is high is a cache
// that is not working rather than one that is wrong.
//
// -- what "correct" means here, exactly ---------------------------------------
//
// This is a SAMPLING source. It answers what a volume at `cell_size` answers,
// and that is a different promise at three kinds of point:
//
//   * ON THE LATTICE the prefix was built for — worst 3.3e-7, float rounding.
//     A seed read there IS the stored sample rather than an interpolation of
//     two, which is what makes the cache usable for Smooth, whose working field
//     is that very lattice. `block_fill()` over the same region gives it,
//     because `sample_blocks` takes its origin straight from `region.min`.
//   * BETWEEN lattice points — worst 7.6e-3 at a 0.03 cell, a quarter of a
//     cell. Ordinary trilinear interpolation error: the same fidelity a
//     consolidation of the same prefix would have, and NOT the walk's answer.
//     A consumer that needs the exact field at arbitrary points wants the walk.
//   * OUTSIDE the prefix's stored bricks — exact, because the far-bound rule
//     sends those windows to the tape.
//
// Two things had to be got right for the first of those, and both were measured
// wrong first, so they are stated rather than left to be rediscovered:
// the prefix is baked WITHOUT redistance (which rewrites every sample: 0.063
// error, two cells), and over the WHOLE LAYER'S region rather than its own
// (which moves the lattice origin: 0.0074 error).

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "clay/field/volume.h"
#include "clay/parallel/cancel.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

namespace clay {
namespace session {

// The digest of the layer's own properties plus roots [0, root_count), and
// NOTHING after them.
//
// A whole-layer digest would be the wrong key: appending a root changes it, and
// the represented prefix did not change at all, so every append would throw
// away a cache that is still perfectly valid. That is the entire reason this
// exists beside `layer_fingerprint`.
//
// `layer_prefix_fingerprint(l, l.sdf->roots.size())` equals
// `layer_fingerprint(l)` by construction, and a test pins it so the two cannot
// drift apart.
std::uint64_t layer_prefix_fingerprint(const scene::Layer& layer, std::size_t root_count);

// Where a boundary may go, and whether it is worth having one.
//
// Deliberately not a document property: a resolution and a memory ceiling
// belong to a device and a session, and storing them in the artwork would put a
// cache policy in the file. A zeroed policy caches NOTHING, which is the safe
// reading of a struct nobody filled in.
struct SdfPrefixPolicy {
    // Required, and > 0: a document has no intrinsic resolution, and guessing
    // one here would fix a cache's fidelity at a number nobody chose. Same
    // meanings as ConsolidationParams throughout.
    float cell_size = 0.0f;
    float band = 0.0f;     // <= 0 means three cells
    float padding = 0.0f;  // <= 0 means the band

    // Below this many roots, do not cache: the walk is already cheap and a
    // volume would cost more memory than it saves time.
    std::size_t min_history_roots = 0;
    // How many roots stay live in front of the boundary. The suffix is what
    // still costs per evaluation, so this is the knob that trades cache
    // rebuild frequency against interactive cost.
    std::size_t keep_live_suffix_roots = 0;
    // 0 DISABLES the cache entirely — not "unbounded". A cache with no ceiling
    // is a leak on a device with a memory budget, and the safe reading of an
    // unset field is "off".
    std::size_t max_bytes = 0;

    // Snap the bake's region origin DOWN to a multiple of `cell_size`.
    //
    // WHICH CONSUMER'S LATTICE THE PREFIX IS FOR, and the two want different
    // ones. A seed read off the lattice it was built for IS the stored sample
    // (3.3e-7); one read off a lattice half a cell away is an interpolation of
    // two (a quarter of a cell, measured 0.011 at a 0.05 cell).
    //
    //   FALSE — the region starts at the layer's padded bounds. The Smooth
    //   transaction's working field is that same region, so the two share a
    //   lattice by construction. This is the default and the original.
    //
    //   TRUE — the origin is a multiple of the cell, which is the lattice a
    //   BRICK refill uses: brick origins are key * dim * voxel, anchored at the
    //   world origin. Snapping only ever moves the origin down by less than a
    //   cell, into the padding, so no sample is lost.
    //
    // It is part of the cache KEY, so the two are different entries and neither
    // is served to the other's consumer. Snapping unconditionally was tried and
    // is wrong: it moves Smooth's prefix off Smooth's lattice, which
    // `test_sdf_prefix_cache.cpp`'s "exact on the lattice it was built for"
    // catches at 0.0149 against its 1e-5 gate.
    bool align_to_lattice = false;
};

struct SdfPrefixCacheStats {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t builds = 0;
    std::uint64_t evictions = 0;
    std::uint64_t invalidations = 0;
    // The composition counters — see the far-bound rule above. A window seeded
    // from the volume is the fast path; one that fell back evaluated the prefix
    // tape because the volume did not store every sample of it.
    std::uint64_t seeded_windows = 0;
    std::uint64_t fallback_windows = 0;
};

// One cached prefix. Owned by the cache; borrowed by whoever is evaluating.
struct SdfPrefixField {
    scene::LayerId layer = 0;
    std::size_t prefix_roots = 0;
    std::uint64_t fingerprint = 0;
    float cell_size = 0.0f;
    float band = 0.0f;
    float padding = 0.0f;
    bool align_to_lattice = false;
    field::FieldVolume volume;

    std::size_t bytes() const { return volume.bytes(); }
};

// Where the boundary should sit for this layer under this policy, or 0 meaning
// "do not cache this layer".
//
// Top-level roots only, and never inside a group: a group is one root and its
// children are not a boundary the fold reaches. That is a P0 restriction rather
// than a permanent one, and it is why this returns a root COUNT and not a node.
std::size_t prefix_boundary_for(const scene::Layer& layer, const SdfPrefixPolicy& policy);

// A memory-budgeted set of cached prefixes, keyed by layer, boundary and
// resolution. Never serialized, never a Node, never visible to undo.
class SdfPrefixCache {
  public:
    SdfPrefixCache() = default;

    // The entry matching this layer at this boundary and resolution, still
    // provably describing roots [0, prefix_roots), or null.
    //
    // The fingerprint is re-checked here rather than trusted from an
    // invalidation call: command-aware invalidation is an optimisation and the
    // digest is the safety net, because a missed invalidation is wrong geometry
    // and a redundant one is only slow.
    const SdfPrefixField* find(const scene::Layer& layer, std::size_t prefix_roots,
                               const SdfPrefixPolicy& policy);

    // The same, for a consumer that can take ANY boundary rather than the
    // policy's exact one -- a brick refill, whose suffix simply grows.
    //
    // WHY THIS EXISTS. `prefix_boundary_for` is roots - keep_live_suffix_roots,
    // so every append moves it by one: during a stroke the policy asks for a
    // boundary the cache has never held, and a lookup by that key misses on
    // every dab. The entry at the older boundary is still perfectly valid --
    // roots [0, K) did not change, an append lands after them -- and a suffix
    // compiled from K instead of K' is longer by the dabs since, which is a few
    // items rather than the whole history. So a stroke keeps hitting.
    //
    // Never returns an entry whose boundary reaches `max_boundary`, because a
    // prefix must leave a suffix for the seed to be folded onto.
    const SdfPrefixField* find_usable(const scene::Layer& layer, std::size_t max_boundary,
                                      const SdfPrefixPolicy& policy);

    // A cheap statement that roots [0, K) cannot have changed since the digest
    // last verified them.
    //
    // The digest is the safety net and stays: `find` recomputes it from what
    // the layer holds right now, so a missed invalidation cannot produce wrong
    // geometry. But it is O(prefix roots) -- measured 13.5 ms at 50,000 -- and
    // a BRICK REFILL asks per frame where a Smooth transaction asks once per
    // gesture. So a caller that holds a monotonic witness of structural change
    // may hand it in, and the digest is then recomputed only when it moves.
    //
    // The witness must advance whenever ANY node in [0, K) could have changed.
    // Passing 0 means "no witness", and the digest runs as it always did.
    void set_structure_witness(std::uint64_t witness) { witness_ = witness; }

    // Build and cache the prefix for `layer` at the policy's boundary. Null
    // when the policy declines, the layer is not the document's last visible
    // SDF layer, or the bake refuses. Cancellable; a cancelled build caches
    // nothing.
    const SdfPrefixField* build(const scene::Document& doc, scene::LayerId layer,
                                const SdfPrefixPolicy& policy,
                                const scene::BakePointEval& point_eval = {},
                                parallel::CancelToken* token = nullptr);

    void invalidate_layer(scene::LayerId layer);
    void clear();

    // Lowering this evicts down to it at once, least-recently-used first.
    void set_max_bytes(std::size_t bytes);
    std::size_t max_bytes() const { return max_bytes_; }

    const SdfPrefixCacheStats& stats() const { return stats_; }
    // The composition counters are written by SdfSourceField, which borrows
    // entries from here and is where the far-bound decision is made.
    void note_seeded(bool fell_back);

  private:
    struct Entry {
        SdfPrefixField field;
        std::list<std::uint64_t>::iterator lru;
        // The witness value the digest last agreed with. 0 means "never
        // verified under a witness", so the digest runs.
        std::uint64_t verified_witness = 0;
    };
    static std::uint64_t key_of(scene::LayerId layer, std::size_t roots,
                                const SdfPrefixPolicy& policy);
    void evict_to_budget();
    bool verify(std::uint64_t key, const scene::Layer& layer, std::size_t prefix_roots);
    std::uint64_t witness_ = 0;

    std::unordered_map<std::uint64_t, Entry> entries_;
    std::list<std::uint64_t> order_;  // front = most recently used
    std::size_t max_bytes_ = 0;
    std::size_t bytes_ = 0;
    SdfPrefixCacheStats stats_;
};

// The layer's field, answered as cheaply as it can be answered correctly.
//
// Opened once against an immutable view of the source, then asked for windows
// of lattice points. Every window is the full walk's answer: where a cached
// prefix covers the window it seeds a suffix, and where it does not the prefix
// tape is evaluated for that window instead.
//
// THE FRAME IS THE LAYER'S OWN, exactly as `bake_layer` samples it — the layer
// visible and its transform identity — so a volume built through this composes
// with, and can be installed under, the layer the same way a consolidation's
// can. That is why this holds its own view of the document rather than reading
// the caller's: the frame is part of what a cached prefix means.
class SdfSourceField {
  public:
    // Null when the layer is missing or holds no SDF content. `cache` may be
    // null, and a null cache is simply the full walk — which is what makes
    // every accelerated path optional rather than load-bearing.
    //
    // NEVER BUILDS. An existing cache entry is used and a missing one is not
    // filled in, because this is the call a Smooth transaction makes at
    // pointer-down and a bake there would be the whole-layer cost the lazy path
    // exists to remove. `SdfPrefixCache::build` is the door for a host that has
    // somewhere to put that work.
    static std::optional<SdfSourceField> open(const scene::Document& doc, scene::LayerId layer,
                                              SdfPrefixCache* cache = nullptr,
                                              const SdfPrefixPolicy& policy = {},
                                              const scene::BakePointEval& point_eval = {},
                                              parallel::CancelToken* token = nullptr);

    SdfSourceField(SdfSourceField&&) = default;
    SdfSourceField& operator=(SdfSourceField&&) = default;
    SdfSourceField(const SdfSourceField&) = delete;
    SdfSourceField& operator=(const SdfSourceField&) = delete;

    // `count` points, `count` distances out. Correct whatever the cache holds.
    void fill_points(const float* points_xyz, std::size_t count, float* out) const;

    // A fill for FieldVolume::sample_blocks, so a bake through this is the bake
    // it would have been.
    field::FieldVolume::BrickBlockFill block_fill() const;

    // What the whole layer's field bounds are, for a caller sizing a region.
    const math::Aabb& bounds() const { return bounds_; }
    // True when a cached prefix is actually being used; false means every
    // window is the full walk.
    bool accelerated() const { return prefix_ != nullptr; }
    std::size_t prefix_roots() const { return prefix_ ? prefix_->prefix_roots : 0; }
    std::size_t suffix_roots() const { return suffix_roots_; }

  private:
    SdfSourceField() = default;
    // One batch, with the coverage question already answered for all of it.
    void fill_span(const float* points_xyz, std::size_t count, float* out, bool covered) const;
    // Whether the cached prefix stores every one of these points.
    bool covers(const float* points_xyz, std::size_t count) const;

    // A shallow copy: Layer holds its SdfContent by shared_ptr, so this shares
    // every node and only overrides visibility and the layer transform. Node
    // ids are therefore the caller's, which is what lets the suffix name them.
    scene::Document view_;
    scene::LayerId layer_ = 0;
    const SdfPrefixField* prefix_ = nullptr;
    SdfPrefixCache* cache_ = nullptr;
    scene::Tape full_;    // the whole layer: the oracle, and the fallback
    scene::Tape prefix_tape_;  // roots [0, K) — the exact seed when the volume cannot
    scene::Tape suffix_;  // roots [K, N), expecting a seed on the stack
    bool composed_ = false;
    std::size_t suffix_roots_ = 0;
    math::Aabb bounds_;
};

}  // namespace session
}  // namespace clay
