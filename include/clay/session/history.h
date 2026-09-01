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
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/topology_delta.h"
#include "clay/scene/commands.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/groups.h"
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
        // A whole gesture on an ADAPTIVE surface: connectivity, geometry and
        // attributes together. Its own kind rather than an overload of Mesh,
        // because `VertexDeltas` deliberately records no indices — the
        // fixed-topology contract paying off — and a payload that can add and
        // remove faces is a different thing reversed a different way.
        DynamicMesh,
        Mask,    // the cells one mask edit changed, on one layer's mask
        // The document's surface groups, before and after one edit. Named
        // SurfaceGroup and not Group because JournalEvent::Kind already spends
        // GroupBegin/GroupEnd on COMMAND grouping, which is an unrelated idea —
        // one bundles edits into a step, the other names a region of the model.
        SurfaceGroup,
        // Every step an explicit begin_group/end_group bracket collected, as
        // ONE step. The command stack already collapses SCENE commands into a
        // single entry; this is the same promise for the kinds it cannot see.
        //
        // It exists because the bracket did not actually bracket. begin_group
        // forwards to UndoStack::begin_group and nothing else, so a voxel,
        // mask or mesh step recorded between the two calls was pushed straight
        // onto the step list and stayed its own undo. A crossing — create a
        // voxel layer, rasterize into it — is exactly that shape, and it undid
        // in two: the layer went, then the fill it contained (#341). clay.h has
        // promised "undo as one step" since the bracket shipped, and for every
        // representation but the edit list the promise was false.
        Compound,
        // ONE LAYER'S WHOLE MESH, before and after. The kind a global voxel
        // remesh needs and no existing one could carry.
        //
        // A SNAPSHOT where `Mesh` above stores a diff, and the reason is the
        // one that made SurfaceGroup a snapshot: a diff has to be a diff OF
        // something, and there is nothing to diff against. `VertexDeltas`
        // deliberately records no indices — the fixed-topology contract paying
        // off — and `TopologyDelta` records the split, collapse and flip an
        // adaptive edit made, which a rebuild from a volume did not make. A
        // remesh discards every vertex and every polygon at once, so the
        // smallest honest record of it is both meshes.
        //
        // EXPENSIVE, and the budget is what makes that acceptable rather than
        // reckless: `step_bytes` counts both meshes, so a history holding a
        // two-million-triangle rebuild evicts to stay inside its budget exactly
        // as it would for any other large payload. A remesh is a handful per
        // session, which is the same frequency argument SurfaceGroup makes.
        MeshReplace,
        // ONE GESTURE ON A SUBDIVISION HIERARCHY: the detail coefficients and
        // cage positions it edited, at the level it was made on.
        //
        // Its own kind rather than an overload of Mesh, and the reason is the
        // one that keeps the payload small. A coarse stroke on a five-level
        // hierarchy moves millions of vertices at the top, and every one of
        // them is `Subdivide(parent) + Detail` — derived state the hierarchy
        // reconstructs. `VertexDeltas` would record all of them; this records
        // what was EDITED, so the step follows the brush rather than the depth.
        Multires,
        // ONE GESTURE ON A SCULPT LAYER: the coefficients and mask weights it
        // changed, in one channel of one hierarchy's stack.
        //
        // Its own kind rather than an overload of `Multires`, and the argument
        // is the one that kind already makes about `Mesh`: the payloads are
        // different things reversed against different owners. A `Multires` step
        // restores the level's BASE detail and the cage; this restores a
        // LAYER's, reached through the surface its id lives on. Folding them
        // would make every apply branch on which half of the payload is
        // populated.
        MultiresLayer,
        // ONE PROPERTY OPERATION on a stack: a rename, a strength, a
        // visibility, a lock, a reorder, an add, a remove, a merge or a bake.
        //
        // A SECOND KIND rather than a tag inside the one above, and the reason
        // is `step_bytes`: the scene-model delta asks for undo memory to be
        // measurable per kind, and a byte accounting can only separate what the
        // kind separates. A strength change is twenty bytes and a stroke is a
        // megabyte; one kind holding both makes the only interesting question
        // about a history's size unanswerable.
        //
        // Property changes being in the history AT ALL is the thing this change
        // does better than the voxel stack, whose renames and strengths are
        // still outside it: an artist who dials a pass from 100% to 40% and
        // then presses undo means the dial, and a history that skipped past it
        // to the stroke before is a history that lied about what it holds.
        MultiresLayerProperty,
        Barrier  // an operation nothing records; not reversible, not silent
    };

    Kind kind = Kind::Scene;
    scene::LayerId layer = 0;                 // Voxel, Mesh, Mask
    std::vector<voxel::VoxelGrid::SculptChange> cells;   // Voxel
    std::vector<voxel::MaskField::MaskChange> mask_cells;  // Mask
    mesh::VertexDeltas deltas;                // Mesh
    mesh::TopologyDelta topology_delta;        // DynamicMesh
    mesh::MultiresDelta multires_delta;        // Multires
    mesh::SculptLayerDelta sculpt_layer_delta;        // MultiresLayer
    mesh::SculptLayerProperty sculpt_layer_property;  // MultiresLayerProperty
    // SurfaceGroup: the whole field, serialised, on each side of the edit.
    //
    // A WHOLE SNAPSHOT where every other kind stores a DIFF, and deliberately.
    // A voxel step diffs because a stroke is hundreds of steps a second and a
    // grid is megabytes. Group edits are the opposite on both counts: naming a
    // region, hiding one, isolating one — a handful in a session — against an
    // RLE-compressed field of a few kilobytes. Diffing here would buy nothing
    // measurable and would need a second changed-cell mechanism to maintain.
    //
    // It also gets something a cell diff would not: one edit can change ids AND
    // the hidden set (isolate does both), and a snapshot reverses both without
    // two payloads that have to stay in step.
    std::vector<std::uint8_t> group_before, group_after;
    // Compound: what the bracket collected, in the order it is REPLAYED —
    // undo walks it backwards, redo forwards.
    //
    // The Scene child, when there is one, is first, and there is never more
    // than one: an open group occupies a single UndoStack entry however many
    // commands land in it, and sync_scene_steps reconciles that to one Scene
    // step. First rather than in the order it was reconciled, because a Voxel,
    // Mask or Mesh child names a LAYER and the Scene child is what creates or
    // removes it — so undoing scene last, and redoing it first, keeps every
    // payload applied while the layer it names is present.
    std::vector<Step> children;
    // MeshReplace: one layer's whole mesh on each side of the rebuild.
    mesh::Mesh mesh_before, mesh_after;
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
    // An adaptive surface, by the layer that holds it.
    //
    // SET ONCE rather than passed to undo, redo and replay like the three
    // above, following `GroupsFor`'s precedent for a different reason: those
    // three are in every caller's signature already, and adding a fourth would
    // break every host compiled against this header to serve a payload most
    // documents never carry. Still a callable rather than a pointer, so the
    // owner is consulted at the moment of use and this cannot outlive what it
    // names.
    using DynamicMeshFor = std::function<mesh::DynamicSurface*(scene::LayerId)>;
    void set_dynamic_resolver(DynamicMeshFor resolver) { dynamic_for_ = std::move(resolver); }
    // A subdivision hierarchy, by the layer that holds it. Set once, for the
    // reason `DynamicMeshFor` gives: adding a fifth parameter to undo, redo and
    // replay would break every host compiled against this header to serve a
    // payload most documents never carry.
    using MultiresFor = std::function<mesh::MultiresSurface*(scene::LayerId)>;
    void set_multires_resolver(MultiresFor resolver) { multires_for_ = std::move(resolver); }
    // The document's surface groups. NOT keyed by layer, because the lattice is
    // per document — which is also why this is set once rather than passed to
    // undo, redo and replay like the three above: those resolve a MAP lookup
    // that can miss, and there is no map here. Still a callable rather than a
    // pointer, so the owner is consulted at the moment of use and this cannot
    // outlive what it names.
    using GroupsFor = std::function<voxel::GroupField*()>;
    void set_groups_resolver(GroupsFor resolver) { groups_for_ = std::move(resolver); }

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

    // A surface-group edit, bracketed. A THIRD mechanism, and the reason is the
    // same one that made the mask a second: GroupField has no single write
    // choke point either — `set` is one, and `reassign`, `grow`, `shrink`,
    // `set_visible`, `isolate`, `show_all` and `invert_visibility` all write
    // their own state, two of them touching only the hidden set and no cell at
    // all. Bracketing the whole edit and comparing serialised fields catches
    // every one of them without eleven call sites having to remember.
    //
    // An edit that changed nothing is dropped, as every other kind is: a host
    // that isolates the group already isolated must not add an undo that does
    // nothing to the menu.
    bool begin_group_step(voxel::GroupField& groups);
    void end_group_step(voxel::GroupField& groups);
    bool recording_group_step() const { return group_open_; }

    // A mesh edit, as the deltas the sculptor already produced. Empty deltas
    // are dropped, for the reason above.
    void record_mesh_step(scene::LayerId layer, mesh::VertexDeltas deltas);
    // One adaptive gesture — every split, collapse, flip and displacement it
    // made — as ONE step.
    void record_dynamic_mesh_step(scene::LayerId layer, mesh::TopologyDelta delta);
    // One gesture on a subdivision hierarchy — every stamp of it — as ONE step.
    // Empty records are dropped, for the reason every other recorder drops a
    // no-op.
    void record_multires_step(scene::LayerId layer, mesh::MultiresDelta delta);
    // One gesture on a SCULPT LAYER — every stamp of it — as ONE step. Empty
    // records are dropped, for the reason every other recorder drops a no-op.
    void record_multires_layer_step(scene::LayerId layer, mesh::SculptLayerDelta delta);
    // One property operation on a stack. Recorded even though it moves no
    // vertex in the rename case, because an artist who renamed a pass and
    // pressed undo means the rename — and a history that skipped past it to the
    // stroke before would take back work the user did not ask to lose.
    void record_multires_layer_property(scene::LayerId layer, mesh::SculptLayerProperty property);
    // One layer's mesh REPLACED wholesale — a global voxel remesh. Both sides
    // are taken by value because both are kept: undo needs the before and redo
    // needs the after, and the layer holds only one of them at a time.
    //
    // A replacement that changed nothing is DROPPED, as every other recorder
    // drops a no-op: a remesh that reproduced its input exactly is not a step a
    // user should have to walk back through.
    void record_mesh_replace(scene::LayerId layer, mesh::Mesh before, mesh::Mesh after);

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
        // GroupBegin/GroupEnd bracket a COMMAND group; SurfaceGroup is an edit
        // to the document's named regions. Two unrelated meanings of "group"
        // that this enum now has to hold at once — spelled apart rather than
        // left to context.
        enum class Kind {
            Command, GroupBegin, GroupEnd, Voxel, Mesh, Mask, SurfaceGroup, Barrier, Undo, Redo,
            // APPENDED, so every enumerator an older journal wrote keeps its
            // value and an old journal still replays. A new kind at the front
            // would renumber the rest and silently reinterpret every event in
            // every file already on disk.
            DynamicMesh,
            MeshReplace,  // appended, for the reason above
            Multires,     // appended, for the reason above
            MultiresLayer,         // appended, for the reason above
            MultiresLayerProperty  // appended, for the reason above
        };
        Kind kind = Kind::Command;
        scene::Command command;                   // Kind::Command
        scene::LayerId layer = 0;                 // Voxel, Mesh
        std::vector<voxel::VoxelGrid::SculptChange> cells;
        std::vector<voxel::MaskField::MaskChange> mask_cells;  // Kind::Mask  // Voxel
        mesh::VertexDeltas deltas;                // Mesh
        mesh::TopologyDelta topology_delta;        // DynamicMesh
        mesh::MultiresDelta multires_delta;        // Multires
        mesh::SculptLayerDelta sculpt_layer_delta;        // MultiresLayer
        mesh::SculptLayerProperty sculpt_layer_property;  // MultiresLayerProperty
        // SurfaceGroup: the field AFTER the edit. Only the after side, unlike
        // the step — a journal replays forward onto the snapshot it was taken
        // against and never runs backwards, so the before side would be bytes
        // nothing reads.
        std::vector<std::uint8_t> group_after;
        // MeshReplace: the mesh AFTER the rebuild, encoded. Only the after
        // side, for the reason group_after gives — a journal replays forward
        // onto the snapshot it was taken against and never runs backwards.
        std::vector<std::uint8_t> mesh_after;
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
    // Fold everything the just-closed bracket produced into one Compound step.
    // Called only from end_group, and only after sync_scene_steps has appended
    // the group's Scene entry, so the whole bracket is in steps_ at or after
    // group_start_.
    void collapse_group();
    // How many Scene steps a step accounts for — 1 for a Scene step, the sum
    // over children for a Compound. The census in sync_scene_steps counts
    // entries on the wrapped stack, and a collapsed group still names one.
    static std::size_t scene_steps_in(const Step& s);

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
    GroupsFor groups_for_;
    DynamicMeshFor dynamic_for_;
    MultiresFor multires_for_;
    bool group_open_ = false;
    std::vector<std::uint8_t> group_snapshot_;
    scene::LayerId open_mask_layer_ = 0;
    bool grouping_ = false;
    // Where steps_ stood when the open bracket began, so end_group knows what
    // the bracket produced. Meaningful only while grouping_.
    std::size_t group_start_ = 0;
    bool enabled_ = false;
};

}  // namespace session
}  // namespace clay
