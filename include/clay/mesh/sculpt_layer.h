#pragma once

// A SCULPT LAYER STACK OVER A SUBDIVISION HIERARCHY (mesh-sculpt-layers spec,
// add-mesh-sculpt-layers).
//
// THE GAP THIS CLOSES, as an artist states it: make a wrinkle pass, then dial
// it back to half, then hide it, then delete it — days later, without redoing
// any of the work under it and without replaying a single stroke. Nothing on a
// mesh could do that. `add-mesh-multires` made a pass SURVIVE an edit beneath
// it; this makes a pass ADDRESSABLE afterwards.
//
// -- THE NAME. `Layer` MEANS THREE THINGS IN THIS LIBRARY -------------------
//
// `MeshBrush::Layer` is a brush ALGORITHM (deposit to a ceiling above the
// surface as the stroke found it). `scene::LayerId` is a DOCUMENT layer, which
// the session history keys every step by. An artist's sculpt layer is a third
// thing entirely, and it is a CHANNEL: named, reorderable, dialable, stored.
//
// So nothing here is ever spelled `Layer` unqualified. Every type is
// `SculptLayer*`, every C entry point is `clay_multires_sculpt_layer_*` — the
// same `sculpt_layer` prefix the voxel stack already spends, so the two artist
// stacks read alike and neither reads like the brush — and the discipline is
// GATED by `tools/check_c_abi.py` rather than remembered. Renaming the brush
// enumerator was the obvious alternative and was rejected: it is shipped in the
// C enum, in the Swift enum and in every host's serialized preset, so renaming
// it would break all three to fix a documentation problem.
//
// -- THE MODEL, and it is one line ------------------------------------------
//
//     E(n) = B(n) + Σ sᵢ · mᵢ(v) · Lᵢ(n, v)
//
// B is the level's own base detail — `MultiresLevel::detail`, unchanged in
// meaning, still what a stroke with no active layer writes and still what
// `detail_checksum` hashes. Lᵢ is layer i's coefficients at that level, in the
// SAME `DetailField`, in the SAME transported frame, at the SAME block size.
// `detail_field.h` promised this in its own header — "a layer will be another
// `DetailField`, composed, not a second mechanism" — and this keeps the promise
// literally. A layer contribution and a base detail coefficient are one
// quantity under two owners, so there is no second displacement representation
// to keep in step with the first.
//
// The hierarchy's evaluation model in `multires.h` is untouched:
// `P(n) = S(n) + Frame(n)·Detail(n)`. Only the meaning of `Detail(n)` widens
// from "the level's field" to "the level's COMPOSED field", and with an empty
// stack the composed field is never allocated and the read is the one it always
// was — so a hierarchy with no layers evaluates bit-identically to before this
// existed, and that is a branch on a per-level pointer rather than a code path.
//
// -- ADDITIVE LAYERS COMMUTE, and the spec says so rather than implying -----
//
// Addition is commutative and associative up to float rounding, and the
// composition above visits a block's layers in stack order once, so reordering
// changes ORGANISATION and not geometry. This is the one place this stack
// differs sharply from the voxel one, which replays CELL WRITES and is
// genuinely order-dependent — there, where two passes touched a cell, moving
// one past the other changes which value survives. Both are correct for what
// they store; stating it is what keeps a host from inventing an ordering rule
// that does not exist here.
//
// -- NOTHING IN THIS CHANGE DIVIDES BY A STRENGTH ---------------------------
//
// It is the trap the whole design is written around, and it appears twice.
// A stroke on a layer at strength 0.5 records its FULL contribution (the write
// path stores a difference, never a residual scaled back down), and merging one
// layer into another SETS the target's composition to the identity it needs
// rather than solving for a coefficient that would carry the ratio of two
// strengths. Both formulas would be undefined at zero, and zero is a state one
// slider reaches.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/mesh/detail_field.h"

namespace clay {
namespace mesh {

// A layer's identity, stable for the life of the document.
//
// NEVER A VECTOR INDEX, and the reason is one operation: reordering. Moving a
// layer down the stack changes every index at or below it, so an index handed
// to a host, written into a file or passed across the C ABI would name a
// different pass after a drag. Ids are minted from a counter that is itself
// serialized, so a save, a load and a reorder leave every id exactly where the
// host left it.
using SculptLayerId = std::uint64_t;
inline constexpr SculptLayerId kNoSculptLayer = 0;
// What `index_of` answers with for an id this stack does not hold.
inline constexpr std::size_t kNoSculptLayerIndex = static_cast<std::size_t>(-1);

// What KIND of thing a layer stores.
//
// VERSIONED FROM THE FIRST RELEASE even though only one kind ships, and written
// per layer into the stream. A reader that meets a kind it does not know
// REFUSES the document rather than skipping the layer: a stream carrying a
// procedural pore pass that a reader silently drops presents a surface missing
// an artist's work while claiming to be complete. Each layer's payload is also
// length-prefixed, so a LATER format can choose to skip deliberately where
// refusing is wrong — the bytes to make that choice exist now and the choice
// itself is not being made now.
enum class SculptLayerKind : std::uint16_t {
    Sampled = 0,     // coefficients stored per vertex; the only kind that ships
    Procedural = 1,  // reserved, and refused by this build's decoder
};

// -- the per-layer mask -------------------------------------------------------

// One weight a vertex, sparse, whose IDENTITY IS ONE.
//
// DISTINCT FROM THE BRUSH GATE, and the distinction is the point rather than a
// naming detail. `field::MaskGate` is a `std::function<float(cfloat3)>`
// evaluated per stamp in world space and owned by the gesture: it says WHERE A
// BRUSH WRITES, and it is gone when the pointer comes up. This says WHERE A
// STORED LAYER CONTRIBUTES — it lives with the layer, is serialized with it, and
// is read at composition time forever after.
//
// The identity is 1 rather than 0, mirroring `DetailField`'s "writing a zero
// releases": a mask the artist has never touched must not erase the layer it
// belongs to, so absent means "contributes fully" and writing 1.0 releases the
// storage again.
//
// A SEPARATE SMALL TYPE rather than a template over `DetailField`. Templating a
// shipped header would re-instantiate it for every file that already includes
// it, to serve one new field — and the two differ in exactly the thing a
// template could not share, their identity element. The blocking, the block
// size and the block INDEX are deliberately identical, because block b naming
// the same 1024 vertices in every field is what makes a strength change cost
// the layer's coverage rather than the surface.
class SparseWeightField {
   public:
    static constexpr std::uint32_t kNoBlock = DetailField::kNoBlock;
    // The ceiling a decoder refuses above, before allocating. Same value and
    // same argument as `DetailField::kMaxVertices`.
    static constexpr std::uint32_t kMaxVertices = DetailField::kMaxVertices;

    void reset(std::uint32_t vertex_count,
               std::uint32_t block_size = DetailField::kDefaultBlockSize);
    std::uint32_t vertex_count() const { return vertex_count_; }
    std::uint32_t block_size() const { return block_size_; }

    // 1.0 outside anything stored.
    float get(std::uint32_t vertex) const;
    // Writing exactly 1.0 releases rather than stores, for the reason above.
    // Values outside [0,1] are clamped: a mask is a proportion, and a host
    // slider that overshoots must fully reveal rather than amplify.
    void set(std::uint32_t vertex, float weight);

    bool empty() const;
    std::size_t resident_vertices() const;
    std::size_t bytes() const;
    // Representation-independent, like `DetailField::checksum`.
    std::uint64_t checksum() const;
    // Release every block that is all ones again.
    void compact();

    // -- block presence, for the composer -------------------------------------
    std::uint32_t block_count() const;
    bool block_stored(std::uint32_t block) const;
    std::uint32_t stored_block_count() const;
    std::uint32_t stored_block_at(std::uint32_t index) const;

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, SparseWeightField* out);

   private:
    std::size_t reserve_slot(std::uint32_t vertex);

    std::uint32_t vertex_count_ = 0;
    std::uint32_t block_size_ = DetailField::kDefaultBlockSize;
    // block -> slot, kNoBlock when the block is untouched and therefore 1.0.
    // Same postcondition, and the same reason, as `DetailField::shrink_to_content`:
    // `bytes()` reports capacity, so a compaction has to release it to be one.
    void shrink_to_content();

    std::vector<std::uint32_t> block_slot_;
    // slot -> block, so a walk over stored blocks costs the stored ones. The
    // same pair `DetailField` keeps, and for the same reason.
    std::vector<std::uint32_t> slot_block_;
    std::vector<float> storage_;
};

// -- one layer ----------------------------------------------------------------

// A channel: what the artist named, what the sliders dial, and the coefficients
// underneath.
//
// PER LEVEL, and most of the per-level fields are empty. A wrinkle pass made at
// level 5 has coefficients at level 5 and nothing anywhere else, and an empty
// `DetailField` costs nothing until something is written into it — which is why
// a layer may span every level of a deep hierarchy and cost only what it
// actually reached.
struct SculptLayer {
    SculptLayerId id = kNoSculptLayer;
    std::string name;
    SculptLayerKind kind = SculptLayerKind::Sampled;

    // How much of what is stored reaches the surface. 1 contributes fully,
    // 0 contributes nothing, and neither replays a stroke — both are read at
    // composition time.
    float strength = 1.0f;
    bool visible = true;
    // A locked layer refuses a SCULPT WRITE and still accepts a PROPERTY
    // change, which is the rule stated rather than discovered: locking exists
    // so an artist can keep working over a finished pass, and a lock that also
    // froze the name and the slider would make "lock" mean "hide from the UI".
    bool locked = false;

    // Index is the level. Sized to the hierarchy; entries are `reset` lazily,
    // the first time something writes at that level.
    std::vector<DetailField> detail;
    std::vector<SparseWeightField> mask;

    // What multiplies this layer's coefficients before they are added, for
    // everything except the mask. Invisible is exactly zero rather than nearly
    // zero, so hiding a layer removes its contribution bit for bit.
    float composition_factor() const { return visible ? strength : 0.0f; }

    // Its coefficients and its mask, allocated. A layer costs its COVERAGE and
    // not the model, which is what makes a hundred passes over one cheek
    // affordable and what a host reports.
    std::size_t bytes() const;
    // Vertices living inside an allocated block, summed over the levels. The
    // storage cost's unit, not how many are non-zero.
    std::size_t coverage_vertices() const;
    bool has_content() const;
};

// What the layer stack costs, split the way `MultiresMemory` splits a
// hierarchy: the artist's work apart from what is derived from it.
//
// REPORTED ON `MultiresMemory` RATHER THAN `io::MemoryReport`, because
// `io::ClaySpaceDoc` holds no hierarchy — a `MultiresSurface` is a standalone
// handle whose bytes a host stores itself, so a document-level row would be a
// claim about an ownership that does not exist.
struct SculptLayerMemory {
    std::size_t content = 0;  // coefficients: authoritative, never droppable
    std::size_t masks = 0;    // also authoritative
    std::size_t total = 0;
    std::size_t layers = 0;
};
// The COMPOSED field is not a row here, deliberately: it belongs to a level
// rather than to a layer, and `MultiresMemory::composed` is where a host that
// is deciding what to drop will look for it. A second, always-zero copy of that
// number on this struct would be a row nobody could act on.

// What composition actually did, so both scale gates are MEASUREMENTS rather
// than claims. There is no other way to see either from outside: a correct
// implementation and a quadratic one produce the same surface.
struct SculptLayerStats {
    // Blocks of `block_size()` vertices recomposed. Task 5.4's gate reads this
    // across a strength change: it must be the layer's allocated blocks and
    // not the level's.
    std::uint64_t blocks_recomposed = 0;
    // (block, layer) pairs actually summed. Task 5.5's gate reads this across a
    // stamp on a deep stack: a layer that does not reach a block is an O(1)
    // miss and is never counted, so the number a stamp on top of 128 layers
    // adds is bounded by the layers that actually cover what it touched.
    std::uint64_t layer_blocks_visited = 0;
    std::uint64_t compositions = 0;  // calls that recomposed at least one block
};

// -- the stack ----------------------------------------------------------------

// Every layer over one hierarchy, bottom-first, plus which one a brush writes
// into and the invalidation bookkeeping each change owes.
//
// OWNED BY `MultiresSurface` rather than held beside it (see `multires.h`).
// Composition happens inside the evaluation, which is called from partial
// evaluation with a vertex list; a stack living outside the surface could only
// participate through a per-vertex callback in that loop, or by having the host
// recompose into `detail_mutable()` — which overwrites the base detail and IS
// the second displacement representation this change exists not to create.
//
// THREE REVISIONS, mirroring the three the surface already has, and for the
// same reason one counter cannot say which of three things happened:
//
//   metadata     a rename, a change of active layer. Invalidates NOTHING.
//   composition  strength, visibility, mask, order, add, remove. Invalidates
//                the blocks that layer has ALLOCATED.
//   content      coefficients written. Invalidates the block written.
//
// Keying the cache on a single stack revision was the simpler alternative and
// was rejected because it makes a RENAME re-evaluate the model.
class SculptLayerStack {
   public:
    // -- shape ----------------------------------------------------------------

    std::size_t size() const { return layers_.size(); }
    bool empty() const { return layers_.empty(); }
    // Bottom-first. Index 0 composites first, and — because addition
    // commutes — that says nothing about geometry and everything about how a
    // host draws the list.
    const SculptLayer* at(std::size_t index) const;
    const SculptLayer* find(SculptLayerId id) const;
    std::size_t index_of(SculptLayerId id) const;
    SculptLayerId id_at(std::size_t index) const;

    SculptLayerId active() const { return active_; }
    bool set_active(SculptLayerId id);

    // -- lifecycle ------------------------------------------------------------

    // A new empty layer on top, full strength and visible, made active.
    // Returns `kNoSculptLayer` while a stroke holds the composition (below).
    SculptLayerId add(std::string name = {});
    // Reinsert a layer at a known index with a known id — what an undo of
    // `remove` needs, and what `decode` builds a stack out of. Refuses an id
    // this stack already holds.
    bool insert(std::size_t index, SculptLayer layer);
    // Discard a layer. Everything else is untouched: the other layers'
    // coefficients, their strengths and their order relative to each other do
    // not change, and no stroke is replayed. What re-evaluates is the removed
    // layer's own coverage.
    bool remove(SculptLayerId id, SculptLayer* out = nullptr);
    // Slide a layer to another position. Organisation only — see the header
    // note on commuting.
    bool move_to(SculptLayerId id, std::size_t index);

    // -- properties -----------------------------------------------------------

    // Metadata: invalidates nothing geometric.
    bool rename(SculptLayerId id, std::string name);
    // Composition: invalidates the layer's allocated blocks and nothing else.
    bool set_strength(SculptLayerId id, float strength);
    bool set_visible(SculptLayerId id, bool visible);
    // Also metadata — a lock is a permission, not a contribution.
    bool set_locked(SculptLayerId id, bool locked);

    // -- content --------------------------------------------------------------

    // The layer's field at a level, sized on first use. Null for an id or a
    // level this stack does not have. Writing through it must be followed by
    // `note_content` for the blocks touched, or the composed cache will not
    // know; `add_detail` below does both.
    DetailField* detail_mutable(SculptLayerId id, std::uint32_t level);
    const DetailField* detail_at(SculptLayerId id, std::uint32_t level) const;
    SparseWeightField* mask_mutable(SculptLayerId id, std::uint32_t level);
    const SparseWeightField* mask_at(SculptLayerId id, std::uint32_t level) const;

    // ADD, never assign: the layered write path stores a DIFFERENCE, so this is
    // the operation it needs and the one that keeps a second stamp over the
    // same vertex accumulating exactly as a second stamp on the base does.
    bool add_detail(SculptLayerId id, std::uint32_t level, std::uint32_t vertex,
                    const LocalDetail& delta);
    bool set_detail(SculptLayerId id, std::uint32_t level, std::uint32_t vertex,
                    const LocalDetail& value);
    bool set_mask(SculptLayerId id, std::uint32_t level, std::uint32_t vertex, float weight);

    // Fold `upper` into the layer below it and discard `upper`.
    //
    // DEFINED BY THE SURFACE IT LEAVES, not by concatenating coefficients. The
    // naive arithmetic solves `L' = L_l + (s_u·m_u)/(s_l·m_l)·L_u` and is
    // undefined at `s_l = 0`, which is a state one slider reaches. So this
    // stores `L' = s_u·m_u·L_u + s_l·m_l·L_l` and sets the target's composition
    // to the IDENTITY it needs — strength 1, visible, mask cleared — which
    // leaves the evaluated surface unchanged by construction, at zero strength
    // and everywhere else.
    //
    // What is lost is real and is named rather than smoothed over: the merged
    // layer's slider no longer scales what the upper layer contributed
    // independently. That is what merging MEANS, and it is why merge is
    // undoable rather than a barrier.
    bool merge_down(SculptLayerId upper);

    // -- what changed ---------------------------------------------------------

    std::uint64_t metadata_revision() const { return metadata_revision_; }
    std::uint64_t composition_revision() const { return composition_revision_; }
    std::uint64_t content_revision() const { return content_revision_; }
    // Composition plus content, which is what the SURFACE's own
    // `detail_revision` and `evaluated_revision` fold in — so a host written
    // against the multires ABI before this existed keeps working without
    // learning a new counter, and a host that wants to be precise reads the
    // three above.
    std::uint64_t geometry_bumps() const { return composition_bumps_ + content_bumps_; }

    // -- sizing and invalidation ----------------------------------------------

    // Tell the stack how big each level is. Called by the surface when a level
    // is added, removed or reconstructed. Sizes that CHANGE discard the layers'
    // fields at that level: a coefficient stored against a vertex count that no
    // longer exists names a different vertex.
    void set_level_sizes(const std::vector<std::uint32_t>& vertices_per_level);
    std::uint32_t level_count() const {
        return static_cast<std::uint32_t>(level_vertices_.size());
    }
    std::uint32_t level_vertex_count(std::uint32_t level) const;
    std::uint32_t level_block_count(std::uint32_t level) const;
    std::uint32_t block_size() const { return block_size_; }

    // Does any layer store anything at this level? What decides whether the
    // level allocates a composed field at all, and therefore what keeps the
    // no-layer path reading the base field through the arithmetic it always
    // used.
    bool reaches_level(std::uint32_t level) const;

    // The blocks whose composition is stale, and the drain for them.
    bool level_all_dirty(std::uint32_t level) const;
    const std::vector<std::uint32_t>& dirty_blocks(std::uint32_t level) const;
    void clear_dirty(std::uint32_t level);
    bool any_dirty() const;
    // Mark everything stale — what a reload, a level rebuild or a freshly
    // allocated composed field needs.
    void dirty_all();
    // One block at one level, which is what a content write owes.
    void note_content(std::uint32_t level, std::uint32_t block);
    // The BASE detail beneath the stack changed in this block, so the composed
    // field is stale there. Not a content change and not a composition one —
    // nothing about a layer moved — so no layer revision moves either, and the
    // surface's own `detail_revision` records it as it always has.
    void invalidate(std::uint32_t level, std::uint32_t block);

    // -- the composition lock -------------------------------------------------
    //
    // A stroke reads the level's evaluated positions, which include every
    // visible layer's contribution — so hiding a layer or moving a slider
    // between two stamps would change what the second stamp sees, and the
    // gesture would be authored against two different surfaces. While a stroke
    // is open the composition is HELD: strength, visibility, mask, order, add,
    // remove and merge refuse and change nothing; rename, lock and set-active
    // still work, because none of them moves a vertex.
    //
    // Refusing rather than deferring until commit. Deferring is defensible and
    // was the alternative; refusing is the one a host can show, because a
    // slider that appears to move and then silently applies later is a worse
    // surprise than one that will not move.
    void hold_composition(bool held) { composition_held_ = held; }
    bool composition_held() const { return composition_held_; }

    const SculptLayerStats& stats() const { return stats_; }
    SculptLayerStats& stats_mutable() { return stats_; }
    void reset_stats() { stats_ = SculptLayerStats{}; }

    SculptLayerMemory memory() const;

    // Release every all-zero coefficient block and every all-identity mask
    // block. NEVER CALLED INSIDE A POINTER EVENT — it walks the stored blocks
    // of every layer, which is proportional to the stack rather than to the
    // dab. The owner calls it between gestures, and a host under memory
    // pressure calls it as the cheapest of the four levers 5.7 names, the other
    // three being merge, bake and delete.
    void compact();

    // -- serialization --------------------------------------------------------
    //
    // Its own versioned form, embedded in the multires stream by the surface.
    // Also what an undo of a STRUCTURAL operation stores on each side — a
    // snapshot rather than a diff, which is the trade `session::Step` already
    // makes for surface groups: a handful in a session, against a diff
    // mechanism that would have to be maintained for operations that reshape
    // the whole list.
    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, SculptLayerStack* out);

    // A ceiling a decoder refuses above BEFORE allocating. A few hundred bytes
    // declaring four billion layers is a request for more memory than a machine
    // holds, and it must be refused by arithmetic rather than by `bad_alloc`.
    static constexpr std::uint32_t kMaxLayers = 1u << 16;
    static constexpr std::uint32_t kMaxNameBytes = 1u << 16;

    // THE SAME TWO CEILINGS THE STREAM AROUND THIS ONE ALREADY APPLIES, and
    // they are here because the stack chunk is read BEFORE the hierarchy it
    // belongs to can be consulted. `MultiresSurface::decode` does check that a
    // decoded stack's levels are this hierarchy's — but only after this
    // function has returned, which is far too late to matter: a level count and
    // a per-level vertex count are numbers this decoder RESERVES FROM, so a
    // stack chunk declaring three levels of four billion vertices reserved
    // three gigabytes of block index before anyone could say it was nonsense.
    //
    // Duplicated rather than included, because `multires.h` includes THIS
    // header and not the other way round; a `static_assert` there holds the two
    // pairs equal, so the copy cannot drift without failing the build.
    static constexpr std::uint32_t kMaxLevels = 12;
    static constexpr std::uint32_t kMaxLevelVertices = DetailField::kMaxVertices;

   private:
    // One level of `merge_down`, and the union of the two layers' coverages it
    // walks. Both are indices rather than ids: `merge_down` has already
    // resolved them and a second lookup could not fail differently.
    void merge_level(std::size_t upper, std::size_t lower, std::uint32_t level, float upper_factor,
                     float lower_factor);
    void merge_blocks(std::size_t upper, std::size_t lower, std::uint32_t level,
                      std::vector<std::uint32_t>* blocks) const;

    struct LevelDirty {
        std::vector<std::uint32_t> blocks;
        std::vector<char> mark;
        bool all = true;
    };

    SculptLayer* mutable_find(SculptLayerId id);
    void note_block(std::uint32_t level, std::uint32_t block);
    // Every block this layer has allocated, at every level — what a composition
    // change to it invalidates. `DetailField::slot_block_` already makes a walk
    // over stored blocks cost the stored ones, which is why task 5.4's gate is
    // a data-structure property here rather than an optimisation.
    void note_layer_coverage(const SculptLayer& layer);
    void size_layer(SculptLayer* layer) const;

    std::vector<SculptLayer> layers_;
    std::vector<std::uint32_t> level_vertices_;
    std::vector<LevelDirty> dirty_;
    std::uint32_t block_size_ = DetailField::kDefaultBlockSize;
    SculptLayerId active_ = kNoSculptLayer;
    // Serialized, so an id minted before a save is never minted again after a
    // load. Starts at 1 because 0 is `kNoSculptLayer`.
    SculptLayerId next_id_ = 1;
    std::uint64_t metadata_revision_ = 1;
    std::uint64_t composition_revision_ = 1;
    std::uint64_t content_revision_ = 1;
    std::uint64_t composition_bumps_ = 0;
    std::uint64_t content_bumps_ = 0;
    bool composition_held_ = false;
    SculptLayerStats stats_;
};

// -- undo ---------------------------------------------------------------------

// What one gesture on a layer changed, sparse and coalesced.
//
// COALESCED PER GESTURE, exactly as `MultiresDelta` and `VertexDeltas` are: a
// vertex touched by a hundred stamps of one stroke is ONE entry, keeping the
// FIRST `before` and the LAST `after`. The record's size follows the vertices
// the stroke reached, not the stamps it took.
class SculptLayerDelta {
   public:
    std::size_t size() const { return detail_.size() + mask_.size(); }
    bool empty() const { return detail_.empty() && mask_.empty(); }
    void clear();
    std::size_t bytes() const;

    // The layer every entry belongs to. A gesture writes ONE layer — which is
    // also what makes a mirrored write under symmetry one layer and one step,
    // with the coverage as the union of the two sides.
    SculptLayerId layer() const { return layer_; }
    void set_layer(SculptLayerId id) { layer_ = id; }
    // The levels this gesture touched, ascending.
    std::vector<std::uint32_t> levels() const;

    void note_detail(std::uint32_t level, std::uint32_t vertex, const LocalDetail& before);
    void note_mask(std::uint32_t level, std::uint32_t vertex, float before);
    // Rewrite every `after` from the stack as it now is, so the last stamp of
    // the gesture wins.
    void sync_after(const SculptLayerStack& stack);

    // Restore / re-apply. Both idempotent, and both refuse — changing
    // nothing — against a stack that no longer holds the layer or whose levels
    // do not match, which is a caller pairing a step with the wrong surface.
    bool revert(SculptLayerStack& stack) const;
    bool apply(SculptLayerStack& stack) const;

    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, SculptLayerDelta* out);

   private:
    struct DetailEntry {
        std::uint32_t level = 0;
        std::uint32_t vertex = 0;
        LocalDetail before, after;
    };
    struct MaskEntry {
        std::uint32_t level = 0;
        std::uint32_t vertex = 0;
        float before = 1.0f, after = 1.0f;
    };
    static std::uint64_t key_of(std::uint32_t level, std::uint32_t vertex) {
        return (static_cast<std::uint64_t>(level) << 32) | vertex;
    }
    bool write(SculptLayerStack& stack, bool forward) const;

    SculptLayerId layer_ = kNoSculptLayer;
    std::vector<DetailEntry> detail_;
    std::vector<MaskEntry> mask_;
    // The slot indices are NOT encoded, for the reason `MultiresDelta` gives
    // for its own: they are derivable from the entries, and storing a hash
    // map's contents would be storing a rebuildable thing.
    std::unordered_map<std::uint64_t, std::uint32_t> detail_slot_, mask_slot_;
};

// What one PROPERTY operation changed, both sides.
//
// A SECOND STEP KIND rather than a tag inside the one above, because the
// scene-model delta asks for undo memory to be measurable PER KIND and a byte
// accounting can only separate what the kind separates. A strength change is
// twenty bytes and a stroke is a megabyte; folding them into one kind makes the
// only interesting question about a history's size unanswerable.
struct SculptLayerProperty {
    enum class Op : std::uint16_t {
        Rename = 0,
        Strength = 1,
        Visible = 2,
        Locked = 3,
        Active = 4,
        // ADD, REMOVE, MOVE, MERGE and BAKE, as the whole stack on each side.
        //
        // A SNAPSHOT where the cheap operations above store a scalar, and the
        // trade is the one `session::Step` already makes for surface groups: a
        // remove or a merge reshapes the list, rewrites another layer's
        // coefficients and can change which layer is active, so a diff would
        // need a mechanism for each and they happen a handful of times in a
        // session — against a strength change, which happens continuously and
        // stores two floats.
        Structural = 5,
    };

    Op op = Op::Strength;
    SculptLayerId layer = kNoSculptLayer;

    std::string name_before, name_after;                  // Rename
    float strength_before = 1.0f, strength_after = 1.0f;  // Strength
    bool flag_before = false, flag_after = false;         // Visible, Locked
    SculptLayerId active_before = kNoSculptLayer, active_after = kNoSculptLayer;  // Active

    // Structural: the encoded stack on each side.
    std::vector<std::uint8_t> stack_before, stack_after;
    // Structural: what the operation wrote OUTSIDE the stack. A bake folds a
    // layer into the base detail, and a bake of a level-0 layer moves the cage
    // itself — neither of which the snapshot above can carry, because neither
    // belongs to the stack.
    struct DetailEntry {
        std::uint32_t level = 0;
        std::uint32_t vertex = 0;
        LocalDetail before, after;
    };
    std::vector<DetailEntry> base_detail;
    std::vector<std::uint32_t> base_vertices;
    std::vector<kernel::cfloat3> base_before, base_after;

    std::size_t bytes() const;
    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, SculptLayerProperty* out);
};

}  // namespace mesh
}  // namespace clay
