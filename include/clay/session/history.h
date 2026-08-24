#pragma once

// ONE UNDO, ACROSS THREE REPRESENTATIONS, and why it is its own module.
//
// This library has three history mechanisms and, until this existed, no single
// undo step spanned two of them:
//
//   SDF edit list, layer state  ->  scene::UndoStack over scene::Command
//   voxel grid                  ->  sculpt layers (an ARTIST-facing stack)
//   mesh layer                  ->  mesh::VertexDeltas
//
// Each is right on its own. A voxel edit has no compact inverse — the inverse
// of "carve here" is the cells that were there — which is why the voxel side
// records passes rather than commands; a vertex displacement is not an edit
// item, so scene::Command has no variant for one. What was missing was never an
// inverse. It was an ORDER across the three.
//
// WHY A MODULE. tools/check_layering.py allows `scene` to include only
// {parallel, kernel, math, field} — not voxel, not mesh — so a history that
// reverses a voxel pass AND a vertex delta cannot live beside UndoStack, and no
// amount of care makes it fit there. `brush` is the only module that already
// sees all three and it is the stroke engine, which earns that position with
// one call needing a mesh and a mask together. So this sits in its own module
// above the three, the way `parallel` got one when the layering rule put the
// thread pool out of the core library's reach.
//
// WHY IT WRAPS RATHER THAN REPLACES. UndoStack needs only `scene` and keeps its
// coalescing and its grouping, both of which are real and representation-
// specific. This dispatches to it. Dragging the command stack up a layer would
// buy nothing.
//
// WHY RESOLVERS ARE PASSED IN. The object that OWNS all three representations
// is io::ClaySpaceDoc, which sits ABOVE this module. Naming it here would be a
// cycle. So undo takes callables that turn a layer id into the grid or mesh it
// names — the same shape the field verbs take a mask as a callable, and for the
// same reason.
//
// WHAT THIS IS NOT. It is an INDEX over the three mechanisms, not a merge of
// their storage. A voxel step is still the cells it changed; a mesh step is
// still sparse vertex deltas; an edit-list step is still a command inverse.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/sculpt.h"
#include "clay/scene/commands.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace session {

// One step, whatever made it. Exactly one payload is meaningful, chosen by
// `kind`.
struct Step {
    enum class Kind {
        Scene,   // one entry on the wrapped UndoStack
        Voxel,   // a recorded run of cell writes on one layer's grid
        Mesh,    // sparse vertex deltas on one layer's mesh
        Mask,    // the cells one mask edit changed, on one layer's mask
        Barrier  // an operation nothing records; not reversible, not silent
    };

    Kind kind = Kind::Scene;
    scene::LayerId layer = 0;                 // Voxel, Mesh, Mask
    std::vector<voxel::VoxelGrid::SculptChange> cells;   // Voxel
    std::vector<voxel::MaskField::MaskChange> mask_cells;  // Mask
    mesh::VertexDeltas deltas;                // Mesh
    std::string barrier;                      // Barrier: what happened, for a host to show

    bool reversible() const { return kind != Kind::Barrier; }
};

class History {
  public:
    // A layer id resolved to the thing it names, or null if it no longer
    // exists. Supplied by the caller because the owner sits above this module.
    using GridFor = std::function<voxel::VoxelGrid*(scene::LayerId)>;
    using MeshFor = std::function<mesh::Mesh*(scene::LayerId)>;
    using MaskFor = std::function<voxel::MaskField*(scene::LayerId)>;

    // Off by default, exactly as the command stack has always been opt-in. A
    // document that never enables it behaves as it did before this existed.
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // -- recording -----------------------------------------------------------

    // An edit-list or layer command. Returns what UndoStack::perform returns.
    // A command that COALESCES into the previous entry adds no step, which is
    // what keeps a stroke of many stamps one undo.
    bool perform(scene::Document& doc, const scene::Command& cmd);
    void begin_group();
    void end_group();

    // A voxel edit, bracketed. Between these the grid's change sink is this
    // history's, so every cell the verbs write is journaled in order. Nested
    // calls are refused rather than nested: a step is one edit.
    //
    // The grid is taken by reference on both sides rather than resolved,
    // because a caller recording a step is holding the grid already.
    bool begin_voxel_step(scene::LayerId layer, voxel::VoxelGrid& grid);
    // Closes the step. A step that changed no cell is DROPPED rather than
    // recorded: a dab that missed every cell is normal here, and an undo that
    // does nothing is exactly what this change exists to remove.
    void end_voxel_step(voxel::VoxelGrid& grid);
    bool recording_voxel_step() const { return voxel_open_; }

    // A mask edit, bracketed. Between these the mask records what it changes,
    // and closing the bracket turns that into a step.
    //
    // A DIFFERENT MECHANISM from the voxel bracket above, and deliberately so:
    // `VoxelGrid::set` is the one choke point every voxel verb funnels through,
    // while a mask's `invert`, `clear`, `expand`, `contract` and `smooth` write
    // chunk data directly and only `fill` and `invert_within` go through
    // `set`. The mask snapshots on its first `touch()` and diffs when the step
    // closes; see MaskField::begin_step.
    bool begin_mask_step(scene::LayerId layer, voxel::MaskField& mask);
    void end_mask_step(voxel::MaskField& mask);
    bool recording_mask_step() const { return mask_open_; }

    // A mesh edit, as the deltas the sculptor already produced. Empty deltas
    // are dropped, for the reason above.
    void record_mesh_step(scene::LayerId layer, mesh::VertexDeltas deltas);

    // An operation NO mechanism records. The examples matter, because the
    // obvious ones are wrong: consolidate IS undoable (it takes an UndoStack
    // and records through the command vocabulary), and rasterizing into a grid
    // IS recorded once a sink is installed, since it writes through `set`.
    //
    // What genuinely is not recorded:
    //
    //  - EVERY MASK EDIT. voxel::MaskField is a FOURTH representation with no
    //    history mechanism at all — twenty mutating entry points across the
    //    ABI and not one command variant. `correct-the-undo-scope` counted
    //    three mechanisms and did not count the thing that has none.
    //  - Operations that destroy history itself: dropping a resolution level,
    //    removing a sculpt layer, merging one down.
    //  - Anything a HOST does that the engine never sees.
    //
    // Recorded so a host can draw a boundary rather than let a user undo
    // through it and be surprised by what survives. Never reversible, never
    // counted in the depths.
    void record_barrier(std::string what);

    // Some engine entry points take a scene::UndoStack* directly and perform
    // commands through it — scene::consolidate_layer is the one today. Hand
    // them this, then call sync_scene_steps() so the session learns how many
    // entries appeared and the step ORDER stays true. Skipping the sync loses
    // the ordering, which is the one way to use this wrongly, so it is named
    // here rather than left to a reader to infer.
    scene::UndoStack* commands() { return &commands_; }
    void sync_scene_steps();

    // -- replay --------------------------------------------------------------

    // Reverse the newest reversible step, whatever produced it. `out_bound`
    // (optional) receives the region a consumer holding a cache must
    // invalidate, for the scene steps that can report one.
    bool undo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
              math::Aabb* out_bound = nullptr, const MaskFor& mask_for = {});
    bool redo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
              math::Aabb* out_bound = nullptr, const MaskFor& mask_for = {});

    // -- the journal (survive-a-crash) ---------------------------------------
    //
    // An APPEND-ONLY log of events, not a view of the step list, and the
    // difference is the whole correctness argument. A host persists steps 0..4,
    // the user undoes step 4, and a journal read off the step list would no
    // longer contain it — but the host's file still does, so a recovery would
    // replay work the user took back. So an undo is an EVENT, recorded like an
    // edit, and replay reproduces the session including the taking-back.
    //
    // Peek rather than drain: the host names the index it has already
    // persisted and the log is untouched, so a failed write can simply be
    // retried. `trim` is the explicit drop, called once the bytes are safe.
    // Events are recorded at the grain the SESSION was driven at, not at the
    // grain the step list ended up with. That is deliberate and it is what
    // makes replay simple: a Scene event is one COMMAND, and replaying it goes
    // back through perform(), so coalescing and grouping reproduce themselves
    // rather than having to be re-derived from a step.
    //
    // The alternative — one event per step, carrying whatever the step holds —
    // does not work for Scene steps at all: a step names an entry on the
    // wrapped UndoStack and does not carry the command, and one entry can be a
    // coalesced stroke or a whole group. Recording commands sidesteps that.
    struct JournalEvent {
        enum class Kind { Command, GroupBegin, GroupEnd, Voxel, Mesh, Mask, Barrier, Undo, Redo };
        Kind kind = Kind::Command;
        scene::Command command;                   // Kind::Command
        scene::LayerId layer = 0;                 // Voxel, Mesh
        std::vector<voxel::VoxelGrid::SculptChange> cells;
        std::vector<voxel::MaskField::MaskChange> mask_cells;  // Kind::Mask  // Voxel
        mesh::VertexDeltas deltas;                // Mesh
        std::string barrier;                      // Barrier
    };

    // Events from `from` onward. `out_now_at` receives the index to pass next
    // time. An index past the end yields nothing and is not an error — that is
    // a host that is already up to date.
    std::vector<std::uint8_t> journal_since(std::size_t from, std::size_t* out_now_at) const;
    // Drop events below `upto`, which the host calls once they are durable.
    // Indices do NOT shift: they are absolute for the life of the session, so a
    // host that trimmed and then asked for an older index is told it is gone
    // rather than handed the wrong events.
    void trim_journal(std::size_t upto);
    std::size_t journal_first() const { return journal_base_; }
    std::size_t journal_next() const { return journal_base_ + journal_.size(); }

    // Apply a journal onto a document that IS the snapshot it was taken
    // against. Stops at the first barrier and says so, rather than continuing
    // and producing a document quietly missing that operation's effect.
    struct ReplayResult {
        std::size_t applied = 0;
        bool stopped_at_barrier = false;
        std::string barrier;
    };
    bool replay(const std::uint8_t* data, std::size_t size, scene::Document& doc,
                const GridFor& grid_for, const MeshFor& mesh_for, ReplayResult* out,
                const MaskFor& mask_for = {});

    // Steps that will actually reverse something. Barriers are excluded, so a
    // host greying a menu item from this never offers an undo that does
    // nothing.
    std::size_t undo_depth() const;
    std::size_t redo_depth() const;

    // Whether an unreversible operation lies immediately beneath the next
    // undo — which is how a host says "you cannot go further back than this"
    // instead of silently skipping it. Empty when there is none.
    const std::string& next_barrier() const;

    // -- what it costs, and bounding it (add-history-budget) -----------------
    //
    // The history had no cap of any kind: no depth limit, no byte accounting,
    // no eviction, no query. The only lever was enable_undo, which is not a
    // lever, it is a light switch. That was survivable while the history held
    // SDF edits alone. It now holds four step kinds and a journal, so it is
    // both larger and harder to predict.
    //
    // WHAT IS EXPENSIVE IS NOT WHAT YOU EXPECT, in both directions:
    //
    //  - The command stack stores INVERSES, so REMOVING an item records a whole
    //    Node — 440 bytes plus its deformer chain and stroke points — while
    //    ADDING one records an id. A session of deletes and a session of adds
    //    cost very differently and nothing told the host which it was in.
    //  - A voxel or mask step is proportional to the cells it CHANGED, so one
    //    big fill can outweigh a thousand dabs.
    //  - A mesh step holds its deltas BY VALUE, which is what makes the step
    //    self-contained and also doubles a mesh stroke.
    //  - The JOURNAL keeps its own copy of every payload, so a session with
    //    crash recovery on holds roughly twice what one without it does.
    struct Bytes {
        std::size_t undo = 0;     // the step list and the command stack under it
        std::size_t redo = 0;
        std::size_t journal = 0;  // the crash-recovery log, which duplicates payloads
        std::size_t total = 0;
        std::size_t undo_steps = 0;
        std::size_t redo_steps = 0;
        std::size_t journal_events = 0;
        // Steps evicted to stay inside the budget, ever. A host shows a horizon
        // from this rather than letting a user hunt for a step that is gone.
        std::size_t dropped_steps = 0;
    };
    Bytes bytes() const;

    // Zero means UNBOUNDED, which is what a host that never sets one gets — so
    // this change cannot alter behaviour under a host that ignores it.
    //
    // The budget bounds UNDO AND REDO ONLY. It deliberately does NOT evict from
    // the journal: those bytes are the host's crash recovery, and dropping them
    // silently would lose exactly what the feature exists to keep. The journal
    // is reported instead, and the host trims it once its bytes are durable —
    // see trim_journal.
    void set_budget(std::size_t bytes);
    std::size_t budget() const { return budget_; }

    // Drop the oldest steps until the history fits in `bytes`, for a host that
    // has just been told by the OS that memory is short and cannot wait for the
    // next edit to trigger eviction. Zero drops everything but the newest step.
    void trim_to(std::size_t bytes);

    // Total recorded steps including barriers, which is what a memory budget
    // counts and what a test asserting "nothing was recorded" reads.
    std::size_t step_count() const { return steps_.size(); }

    void clear();

  private:
    // Push a step and discard the redo side, which is what any new edit does.
    void push(Step step);
    bool apply_step(const Step& step, bool forward, scene::Document& doc,
                    const GridFor& grid_for, const MeshFor& mesh_for, math::Aabb* out_bound,
                    const MaskFor& mask_for);

    scene::UndoStack commands_;
    std::vector<Step> steps_;
    std::vector<Step> redo_;
    std::vector<JournalEvent> journal_;
    // The absolute index of journal_[0]. Absolute so a trimmed host asking for
    // an old index is refused rather than served the wrong events.
    std::size_t journal_base_ = 0;
    std::size_t budget_ = 0;  // 0 = unbounded
    std::size_t dropped_steps_ = 0;

    // Evict from the oldest end until the budget is met. Never drops the most
    // recent step: a budget must not be able to make the next undo fail.
    void enforce_budget();
    static std::size_t step_bytes(const Step& s);
    static std::size_t event_bytes(const JournalEvent& e);
    std::vector<voxel::VoxelGrid::SculptChange> open_cells_;
    scene::LayerId open_layer_ = 0;
    bool voxel_open_ = false;
    bool mask_open_ = false;
    scene::LayerId open_mask_layer_ = 0;
    bool grouping_ = false;
    bool enabled_ = false;
};

}  // namespace session
}  // namespace clay
