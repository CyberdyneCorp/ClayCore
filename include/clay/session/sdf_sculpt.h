#pragma once

// Transient SDF sculpt transactions (sdf-sculpt-transaction spec): the
// lifetime a FIELD or DEFORMATION brush needs and an edit-list brush does not.
//
// -- why an ordinary stroke is not this -------------------------------------
//
// brush/stroke.h turns pointer samples into stamps and stamps into ordinary
// nodes. That is the right shape for every declarative SDF brush and this file
// does not change it: a dab IS a persistent node, so undo, picking,
// serialization and the C ABI already understand the whole gesture and the
// document is never in a state the format cannot describe.
//
// Two verbs cannot be spelled that way, and both are core sculpting:
//
//   * SMOOTH averages the assembled field. There is no node that means "the
//     average of what was here", so relax BAKES — it samples the layer and
//     hands back a volume (field/relax.h says so in its first paragraph). A
//     host with nowhere to keep that volume between pointer events has one
//     option per dab: sample the whole layer, relax, throw it away. That costs
//     the MODEL per dab, so the only affordable implementation is to run it
//     once at pointer-up, which is why Smooth had no live preview at all.
//
//   * MOVE warps the assembled surface, and a warp is a deformer on each item
//     it reaches. Written straight into the document per pointer event, a drag
//     churns revisions, tapes, caches and picking sixty times a second to
//     produce one edit — and `moved_chain`'s leading-grab replacement, which
//     stops the chain growing per frame, only papers over the churn.
//
// -- what a transaction is --------------------------------------------------
//
//     begin    capture the source identity; build the transient state ONCE
//     update   mutate only the transient state; report what went dirty
//     commit   one persistent command group; then, optionally, one policy
//              consolidation inside the same group
//     cancel   nothing persistent ever happened
//
// Between begin and commit the Document is untouched: no nodes, no deformers,
// no undo entries, and a serialization taken mid-gesture is byte-for-byte the
// one taken before it. That is the property the whole file exists to provide,
// and it is what the tests assert first.
//
// -- why `session/` ---------------------------------------------------------
//
// This state is ephemeral, belongs to one gesture, and must never be saved. A
// document that could hold an open stroke would need a format that describes
// one, and a "currently smoothing" field in a `.clayspace` is a state a loader
// would have to decide what to do with. `scene::Document`, `scene::Layer`,
// `StrokePreset` and `FieldVolume` are all the wrong owners for the same
// reason. `session/` is where the things that live for a sitting already live.
//
// -- the resolution is the CALLER'S -----------------------------------------
//
// A document has no intrinsic sampling resolution — `ConsolidationParams` says
// why, and this file keeps that promise rather than inventing a cell size for
// Smooth. `SdfSculptPolicy` carries the same three numbers with the same
// meanings; a host that already chooses them for its Smooth brush routes those
// exact values in.

#include <cstdint>
#include <optional>
#include <vector>
#include <algorithm>

#include "clay/brush/move.h"
#include "clay/field/relax.h"
#include "clay/field/volume.h"
#include "clay/parallel/cancel.h"
#include "clay/scene/commands.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/document.h"
#include "clay/session/sdf_prefix_cache.h"

namespace clay {
namespace session {

// A digest of everything about a layer that an edit could change.
//
// A transaction is built against a SPECIFIC source state and holds it for the
// length of a gesture. If something else edits that layer in the meantime —
// another tool, a replayed journal, an undo — committing the transaction would
// silently overwrite that edit with a preview computed from a document that no
// longer exists. So the source is stamped at begin and re-checked at commit.
//
// A HASH rather than a stored counter, because there is no counter: nothing in
// `scene` bumps a generation today, and adding one would mean every mutating
// entry point in the codebase remembering to. A digest is computed from the
// content, so it cannot be forgotten, and it is paid exactly twice per gesture
// — never per pointer event, which is the cost the spec rules out.
//
// Volume and gate payloads are folded in by IDENTITY (the shared pointer) and
// by size, not by hashing megabytes of samples: they are immutable once shared
// and a replacement is a different object. Collisions are possible in
// principle, as with any 64-bit digest; the consequence is a stale commit
// rather than a corrupt one, and the alternative costs a full compare.
std::uint64_t layer_fingerprint(const scene::Layer& layer);

// When a host is willing to spend an artist's parametric history to keep the
// marcher affordable — and it is never the engine's decision.
//
// `scene::report_layer` measures and never bakes, deliberately: a bake
// discards the parameters of everything it absorbs, and a library that did
// that unasked would be deciding on an artist's behalf that a sphere's radius
// is no longer editable. That contract is unchanged. This adds the OTHER half,
// which was missing: a place for a session to say "in this sculpt mode, on
// this device, with this frame budget, collapsing is acceptable".
//
// Zero disables a criterion, so a value-initialised policy authorises nothing
// and measures nothing — the safe reading of an empty struct.
struct SdfSculptComplexityPolicy {
    // The two degradation mechanisms consolidate.h names separately, plus the
    // size of the list. Kept apart for the reason the report keeps them apart:
    // a chain of grabs and a chain of resampled volumes decay the same number
    // for different reasons, and an aggregate cannot say which to cure.
    float min_safe_step_scale = 0.0f;
    int max_deformer_chain = 0;
    int max_item_count = 0;

    // Off by default. Over budget with this false is a REPORT: the transaction
    // says so and changes nothing.
    bool allow_consolidation = false;
    // Where the consolidation would resample, if it is authorised. A zeroed
    // cell size falls back to the sculpt policy's own, which is the resolution
    // the host already chose for this gesture.
    scene::ConsolidationParams consolidation;
};

// The sampling a Smooth gesture works at, with `ConsolidationParams` meanings:
// `cell_size` is required, `band` <= 0 means three cells, `padding` <= 0 means
// the band.
struct SdfSculptPolicy {
    float cell_size = 0.0f;
    float band = 0.0f;
    float padding = 0.0f;

    SdfSculptComplexityPolicy complexity;
    // Where an old prefix may be cached, for the materialization a gesture
    // does. The three sampling numbers above are copied over it, so a caller
    // cannot ask for a prefix at a resolution the gesture is not using — a
    // seed off a different lattice is an interpolation rather than the stored
    // sample, which is the whole point of sharing one.
    //
    // A zeroed one caches nothing, and then every fill is the full walk:
    // slower, and identical.
    SdfPrefixPolicy prefix;
};

// What one update changed, so a host invalidates a region rather than a model.
//
// The bounds and the count are GEOMETRIC — what the brush selected, not which
// samples happened to move — so they are reproducible for a given brush over a
// given lattice however much unrelated model surrounds it. `changed` is the
// value question, kept apart because a dab whose weight came out zero
// everywhere still selects its bricks and has nothing to redraw.
struct SdfSculptDirty {
    math::Aabb bounds;
    // For SMOOTH, the bricks the dab rewrote. For MOVE, the ITEMS the drag
    // moves: a drag has no lattice of its own, and the number a host wants
    // there is the same one — how much of the model this frame costs.
    std::size_t touched_bricks = 0;
    bool changed = false;
};

// What the budget said after a committed stroke, and what was done about it.
struct SdfSculptBudget {
    scene::FieldReport report;
    // Over at least one enabled criterion. Advice on its own.
    bool over_budget = false;
    // The layer was collapsed, inside the stroke's own undo group.
    bool consolidated = false;
    scene::ConsolidationCost cost;
};

// The pure decision. No document, no side effects — a host can ask it about a
// report it obtained itself, and the transactions ask it about theirs.
bool over_sculpt_budget(const SdfSculptComplexityPolicy& policy,
                        const scene::FieldReport& report);

// What a gesture's materialization has cost so far, in counts rather than
// clocks — the form a scaling claim can be tested in.
struct SdfSmoothMaterializationStats {
    std::size_t materialized_bricks = 0;  // brought into the working field
    std::size_t reused_bricks = 0;        // already there when a dab asked
    std::size_t updates = 0;
};

// -- Smooth ------------------------------------------------------------------
//
// The layer's field is materialized LAZILY, around the brush, as dabs ask for
// it. `begin()` evaluates nothing.
//
// It used to sample the whole finite layer at pointer-down, which was the
// honest first version of this: a patch needs a rule for what it means where it
// meets the field it was cut from, and that rule is new correctness surface. It
// is now written down. The working field is a volume on the layer's own
// lattice with NO stored samples, and a brick that stores samples is exactly a
// brick that has been materialized — so:
//
//   * a dab materializes the bricks its relax will read, which is its rewrite
//     region plus the stencil's reach, and nothing else;
//   * `rewrite_region` writes only bricks that store samples, so an
//     unmaterialized brick is skipped rather than filled with a guess;
//   * a later dab over the same place materializes nothing, and one that
//     reaches past it materializes only the new bricks.
//
// The thing that would be silently wrong is treating "nobody has asked for this
// brick yet" as "empty space here", which is what `kBrickEmpty` means to every
// other reader of a volume. That is why materialization FORCE-stores: stored-ness
// is the record of what has been filled in, and the dependency halo is derived
// from `relax`'s own stencil rather than guessed at.
//
// COMMIT assembles the whole layer once — see `commit()` for the one semantic
// difference that buys, which is real and stated rather than hidden.
class SdfSmoothTransaction {
  public:
    // Null on: no such layer, not an SDF layer, no edit list, a protected
    // layer, a cell size of zero, or a layer whose field could not be sampled
    // (empty, unbounded, or cancelled through `token`).
    // Evaluates NOTHING: a compile of the layer, an index for the working
    // lattice, and a digest. `cache` may be null; when it holds a prefix for
    // this layer the materialization each dab pays is the suffix's rather than
    // the whole history's, and when it does not every fill is the full walk.
    // A cache is never BUILT here — that is a host's to schedule.
    static std::optional<SdfSmoothTransaction> begin(
        scene::Document& doc, scene::LayerId layer, const SdfSculptPolicy& policy,
        const scene::BakePointEval& point_eval = {}, parallel::CancelToken* token = nullptr,
        SdfPrefixCache* cache = nullptr);

    SdfSmoothTransaction(SdfSmoothTransaction&&) = default;
    SdfSmoothTransaction& operator=(SdfSmoothTransaction&&) = default;
    SdfSmoothTransaction(const SdfSmoothTransaction&) = delete;
    SdfSmoothTransaction& operator=(const SdfSmoothTransaction&) = delete;

    // One live dab. Relaxes the working volume in place; touches no document
    // state whatsoever. Cancellation is the in-place contract: whole passes
    // only, and the passes already applied stay applied.
    SdfSculptDirty update(const field::RelaxSettings& settings,
                          parallel::CancelToken* token = nullptr);

    // What the host draws, materialized so far. Bricks nobody has reached read
    // as sample-free; the dirty bounds from `update` say which part is new.
    const field::FieldVolume& preview_volume() const { return working_; }

    const SdfSmoothMaterializationStats& materialization() const { return materialized_; }

    // -- the preview delta ----------------------------------------------------
    //
    // Which bricks of the preview hold bytes a consumer has not seen: the ones
    // a dab materialized, and the ones a dab's relax actually moved. A host
    // patching a GPU cache wants these; copying the whole working volume every
    // frame is what this exists to replace.
    //
    // Deduplicated across dabs and held until taken, so a host that skips a
    // frame does not lose one. `take_preview_delta` hands the list over and
    // clears it; `preview_delta` looks without taking.
    const std::vector<field::FieldVolume::BrickCoord>& preview_delta() const { return dirty_; }
    void take_preview_delta(std::vector<field::FieldVolume::BrickCoord>* out);

    // Bumped by every update that changed the preview, and by nothing else. A
    // host uses it to tell a duplicate read from a skipped frame, and to drop
    // an upload it started against an older one.
    std::uint64_t preview_generation() const { return generation_; }
    // Whether any update actually moved a stored sample. A gesture that changed
    // nothing commits without replacing the layer — see commit().
    bool changed() const { return changed_; }

    scene::LayerId layer() const { return layer_; }
    // False once commit or cancel has run. A dead transaction updates nothing
    // and commits nothing.
    bool live() const { return doc_ != nullptr; }

    // Assemble the layer's final volume once and install it as its one item, as
    // ONE undo step, then evaluate the complexity policy inside that same step.
    //
    // A LOCAL working field is not a layer, so commit samples the whole layer
    // through the same source the dabs materialized from, overlays the samples
    // the dabs actually changed, and post-processes once. It does NOT re-run
    // Smooth, and it does not re-evaluate anything a dab already paid for
    // inside the edited region.
    //
    // THE ONE SEMANTIC DIFFERENCE, stated because it is real: the old
    // whole-layer path relaxed a REDISTANCED bake, and this redistances a
    // relaxed field. Both are sound signed distance fields and neither is an
    // approximation of the other; they are not byte-identical, and
    // test_sdf_smooth_lazy.cpp measures how far apart they are rather than
    // asserting they are not.
    //
    // A gesture that changed NOTHING — no dab, strength zero, a mask that
    // froze everything — installs nothing at all: no volume, no undo entry, no
    // consolidation. Pointer-down and pointer-up with no effect in between must
    // not cost an artist their parametric history.
    //
    // Fails, changing nothing, when the source layer has been edited since
    // begin: an external edit is not something a preview may overwrite. Fails
    // the same way on a layer that has since been removed or protected.
    //
    // `undo` may be null, exactly as every other editing entry point allows,
    // and then the commands are applied without being recorded.
    bool commit(scene::UndoStack* undo = nullptr);

    // What the policy found at the last commit. Meaningful after a commit that
    // returned true.
    const SdfSculptBudget& budget() const { return budget_; }

    // Discard. The document was never touched, so this only ends the lifetime.
    void cancel();

  private:
    SdfSmoothTransaction() = default;

    // The dependency region one dab reads, derived from relax's own stencil
    // rather than guessed: the ball it rewrites, plus the stencil's reach,
    // plus a brick for the outward rounding rewrite_region does.
    field::FieldVolume::Region dependency_region(const field::RelaxSettings& settings) const;
    // Fold the coordinates appended since `from` against everything already
    // held, in place.
    void dedup_delta(std::size_t from);

    scene::Document* doc_ = nullptr;
    scene::LayerId layer_ = 0;
    SdfSculptPolicy policy_;
    scene::BakePointEval point_eval_;
    std::uint64_t fingerprint_ = 0;
    // The immutable source this gesture began against. Holds its own view of
    // the document, so later edits to the caller's cannot be sampled by
    // accident — the stale check at commit is what refuses them.
    std::optional<SdfSourceField> field_source_;
    field::FieldVolume working_;
    math::Aabb region_;      // the lattice both the working field and commit use
    math::Aabb edited_;      // union of what the dabs actually changed
    bool changed_ = false;
    std::uint64_t generation_ = 0;
    // Reused across dabs rather than allocated per dab, which is the whole
    // reason the field layer appends to a caller's vector.
    std::vector<field::FieldVolume::BrickCoord> dirty_;
    std::vector<std::uint64_t> dirty_seen_;  // packed coords, sorted; for the dedup
    SdfSmoothMaterializationStats materialized_;
    SdfSculptBudget budget_;
};

// -- Move --------------------------------------------------------------------
//
// The affected items and their frames found ONCE at pointer-down, the preview
// rebuilt per frame from the immutable pre-stroke chains plus the grabs for
// the CURRENT TOTAL displacement — one per image of the drag that reaches the
// item, so under a layer mirror the copy under the ball moves too (see
// brush/move.h) — and every final chain written as one undo step at
// pointer-up.
//
// TOTAL, never incremental. Updates of 0.10, 0.20, 0.50 must end at exactly
// what a single fresh drag of 0.50 produces, not at a composition of three
// warps each authored against a different intermediate surface.
class SdfMoveTransaction {
  public:
    // Null on: no such layer, not an SDF layer, no edit list, a protected
    // layer, or a non-positive radius. A drag that reaches NOTHING is a valid
    // transaction with no affected items — the artist pressed on empty space,
    // which is not an error.
    //
    // `point_eval` is used ONLY if the complexity policy authorises a
    // consolidation after the commit — a drag itself never samples a field.
    // It is here so that when one does fire, mid-session, it is the pooled
    // bake the host already injects everywhere else rather than a serial walk
    // the artist waits through.
    static std::optional<SdfMoveTransaction> begin(scene::Document& doc, scene::LayerId layer,
                                                   kernel::cfloat3 world_centre,
                                                   const brush::MoveSettings& settings,
                                                   const SdfSculptPolicy& policy = {},
                                                   const scene::BakePointEval& point_eval = {});

    SdfMoveTransaction(SdfMoveTransaction&&) = default;
    SdfMoveTransaction& operator=(SdfMoveTransaction&&) = default;
    SdfMoveTransaction(const SdfMoveTransaction&) = delete;
    SdfMoveTransaction& operator=(const SdfMoveTransaction&) = delete;

    // The drag so far, measured from the anchor. O(affected items): no scene
    // traversal, because the traversal happened at begin.
    SdfSculptDirty update(kernel::cfloat3 total_world_displacement);

    // A private copy of the layer with the affected chains replaced — ordinary
    // scene semantics, so the host compiles, draws and picks it exactly as it
    // does the real one. Valid until commit or cancel.
    const scene::Layer& preview_layer() const { return preview_; }

    scene::LayerId layer() const { return layer_; }
    bool live() const { return doc_ != nullptr; }
    // The items the drag reaches. Constant for the gesture — that is the point.
    std::size_t affected_count() const { return affected_.size(); }
    // ...and which ones, in preparation order. For a host that draws its own
    // preview rather than compiling `preview_layer()`.
    const std::vector<scene::NodeId>& affected_nodes() const { return affected_ids_; }
    // The grabs the last update resolved for one of them, in THAT NODE'S own
    // frame — the deformers that belong at the front of its chain, in chain
    // order. One under no symmetry; one per image of the drag that reaches the
    // node under a mirror or a radial count. False for a node this drag does
    // not reach.
    bool preview_grabs(scene::NodeId node, std::vector<scene::Deformer>* out) const;
    // What preparing the drag walked, for a scaling test.
    const brush::MovePrepareStats& prepare_stats() const { return prepare_stats_; }
    // Nodes the LAST update looked at. Must equal affected_count(), whatever
    // the rest of the document holds.
    std::size_t last_update_visited() const { return last_update_visited_; }

    // One SetDeformersCmd per affected item, all inside one undo step, then
    // the complexity policy inside the same step. Fails and changes nothing if
    // the source layer moved under the transaction.
    bool commit(scene::UndoStack* undo = nullptr);

    const SdfSculptBudget& budget() const { return budget_; }
    void cancel();

  private:
    SdfMoveTransaction() = default;

    struct Affected {
        scene::NodeId id = scene::kNoNode;
        brush::PreparedMove prepared;
        // Resolved into per frame rather than returned, so the grab vectors
        // keep their capacity and a frame allocates nothing per item for the
        // resolve itself.
        brush::MoveWarp warp;
        std::vector<scene::Deformer> original_chain;
        std::vector<scene::Deformer> preview_chain;
    };

    scene::Document* doc_ = nullptr;
    scene::LayerId layer_ = 0;
    SdfSculptPolicy policy_;
    scene::BakePointEval point_eval_;
    brush::MoveSettings settings_;
    kernel::cfloat3 anchor_ = kernel::cf3(0, 0, 0);
    kernel::cfloat3 displacement_ = kernel::cf3(0, 0, 0);
    std::uint64_t source_ = 0;
    std::vector<Affected> affected_;
    // The same ids, flat, so a host asking for them is not asking this to
    // build a vector per frame.
    std::vector<scene::NodeId> affected_ids_;
    brush::MovePrepareStats prepare_stats_;
    std::size_t last_update_visited_ = 0;
    scene::Layer preview_;
    SdfSculptBudget budget_;
};

}  // namespace session
}  // namespace clay
