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

namespace clay {
namespace session {

// One step, whatever made it. Exactly one payload is meaningful, chosen by
// `kind`.
struct Step {
    enum class Kind {
        Scene,   // one entry on the wrapped UndoStack
        Voxel,   // a recorded run of cell writes on one layer's grid
        Mesh,    // sparse vertex deltas on one layer's mesh
        Barrier  // an operation nothing records; not reversible, not silent
    };

    Kind kind = Kind::Scene;
    scene::LayerId layer = 0;                 // Voxel, Mesh
    std::vector<voxel::VoxelGrid::SculptChange> cells;   // Voxel
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

    // A mesh edit, as the deltas the sculptor already produced. Empty deltas
    // are dropped, for the reason above.
    void record_mesh_step(scene::LayerId layer, mesh::VertexDeltas deltas);

    // An operation NO mechanism records — a consolidate, a rasterize, a
    // conversion between representations. Recorded so a host can draw a
    // boundary rather than let a user undo through it and be surprised by what
    // survives. Never reversible, never counted in the depths.
    void record_barrier(std::string what);

    // -- replay --------------------------------------------------------------

    // Reverse the newest reversible step, whatever produced it. `out_bound`
    // (optional) receives the region a consumer holding a cache must
    // invalidate, for the scene steps that can report one.
    bool undo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
              math::Aabb* out_bound = nullptr);
    bool redo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
              math::Aabb* out_bound = nullptr);

    // Steps that will actually reverse something. Barriers are excluded, so a
    // host greying a menu item from this never offers an undo that does
    // nothing.
    std::size_t undo_depth() const;
    std::size_t redo_depth() const;

    // Whether an unreversible operation lies immediately beneath the next
    // undo — which is how a host says "you cannot go further back than this"
    // instead of silently skipping it. Empty when there is none.
    const std::string& next_barrier() const;

    // Total recorded steps including barriers, which is what a memory budget
    // counts and what a test asserting "nothing was recorded" reads.
    std::size_t step_count() const { return steps_.size(); }

    void clear();

  private:
    // Push a step and discard the redo side, which is what any new edit does.
    void push(Step step);
    bool apply_step(const Step& step, bool forward, scene::Document& doc,
                    const GridFor& grid_for, const MeshFor& mesh_for, math::Aabb* out_bound);

    scene::UndoStack commands_;
    std::vector<Step> steps_;
    std::vector<Step> redo_;
    std::vector<voxel::VoxelGrid::SculptChange> open_cells_;
    scene::LayerId open_layer_ = 0;
    bool voxel_open_ = false;
    bool enabled_ = false;
};

}  // namespace session
}  // namespace clay
