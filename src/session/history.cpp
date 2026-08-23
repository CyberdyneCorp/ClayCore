#include "clay/session/history.h"

#include <utility>

namespace clay {
namespace session {

namespace {
const std::string kNoBarrier;
}  // namespace

void History::push(Step step) {
    steps_.push_back(std::move(step));
    // Any new edit invalidates redo, across representations. Keeping it would
    // let a redo replay a voxel run onto cells a later SDF edit has moved.
    redo_.clear();
}

bool History::perform(scene::Document& doc, const scene::Command& cmd) {
    if (!enabled_) return scene::apply(doc, cmd).has_value();
    // Coalescing lives in UndoStack and must keep working: a stroke of many
    // stamps is one entry there and must be one step here. So the step is
    // pushed only when the stack actually GREW — which is also what makes an
    // open group add nothing until it closes.
    const std::size_t before = commands_.undo_depth();
    const bool ok = commands_.perform(doc, cmd);
    if (ok && commands_.undo_depth() > before) {
        Step step;
        step.kind = Step::Kind::Scene;
        push(std::move(step));
    }
    return ok;
}

// UndoStack::begin_group pushes its entry IMMEDIATELY, so the stack's depth
// grows here rather than at end_group, and the commands inside append to that
// entry without growing it further. So neither the group's own commands nor
// end_group can be detected by "did the depth grow across this call" — which is
// what perform() uses, and what made a grouped edit record no step at all.
//
// The reconciliation below is the fix: after the group closes, the stack is the
// truth and the step list learns what it missed. A group that stayed empty is
// popped by end_group, so it correctly yields no step.
void History::begin_group() {
    grouping_ = true;
    commands_.begin_group();
}

void History::end_group() {
    commands_.end_group();
    grouping_ = false;
    sync_scene_steps();
}

// Reconcile after an engine call that performed commands through commands().
// The stack is the truth; the step list learns what it missed.
void History::sync_scene_steps() {
    if (!enabled_) return;
    // An OPEN group already occupies an entry on the stack and is not a step
    // until it closes. Reconciling here would record it early.
    if (grouping_) return;
    std::size_t counted = 0;
    for (const Step& s : steps_)
        if (s.kind == Step::Kind::Scene) ++counted;
    const std::size_t actual = commands_.undo_depth();
    for (; counted < actual; ++counted) {
        Step step;
        step.kind = Step::Kind::Scene;
        push(std::move(step));
    }
    // Fewer entries than steps means the last command COALESCED into the one
    // before it, so the newest Scene step no longer names an entry of its own.
    // Removing the newest is correct because coalescing merges backwards.
    for (; counted > actual; --counted) {
        for (std::size_t i = steps_.size(); i > 0; --i) {
            if (steps_[i - 1].kind == Step::Kind::Scene) {
                steps_.erase(steps_.begin() + static_cast<std::ptrdiff_t>(i - 1));
                break;
            }
        }
    }
}

bool History::begin_voxel_step(scene::LayerId layer, voxel::VoxelGrid& grid) {
    if (!enabled_) return false;
    // Nested is refused rather than nested: a step is one edit, and a caller
    // that opened two has a bug this should surface rather than absorb.
    if (voxel_open_) return false;
    open_cells_.clear();
    open_layer_ = layer;
    voxel_open_ = true;
    grid.set_change_sink(&open_cells_);
    return true;
}

void History::end_voxel_step(voxel::VoxelGrid& grid) {
    if (!voxel_open_) return;
    grid.set_change_sink(nullptr);
    voxel_open_ = false;
    // A step that changed no cell is dropped. A dab that misses every cell is
    // ordinary here — a sub-cell grab, a flatten meeting a flat region, a
    // dithered stamp — and recording one would be an undo that does nothing,
    // which is the thing this whole change exists to remove.
    if (open_cells_.empty()) return;
    Step step;
    step.kind = Step::Kind::Voxel;
    step.layer = open_layer_;
    step.cells = std::move(open_cells_);
    open_cells_.clear();
    push(std::move(step));
}

void History::record_mesh_step(scene::LayerId layer, mesh::VertexDeltas deltas) {
    if (!enabled_ || deltas.empty()) return;
    Step step;
    step.kind = Step::Kind::Mesh;
    step.layer = layer;
    step.deltas = std::move(deltas);
    push(std::move(step));
}

void History::record_barrier(std::string what) {
    if (!enabled_) return;
    Step step;
    step.kind = Step::Kind::Barrier;
    step.barrier = std::move(what);
    push(std::move(step));
}

bool History::apply_step(const Step& step, bool forward, scene::Document& doc,
                         const GridFor& grid_for, const MeshFor& mesh_for,
                         math::Aabb* out_bound) {
    switch (step.kind) {
        case Step::Kind::Scene:
            return forward ? commands_.redo(doc, out_bound) : commands_.undo(doc, out_bound);
        case Step::Kind::Voxel: {
            voxel::VoxelGrid* grid = grid_for ? grid_for(step.layer) : nullptr;
            // The layer is gone. Refused rather than skipped: skipping would
            // take the step off the stack and leave the user's next undo
            // reversing something older than they asked for.
            if (!grid) return false;
            if (forward)
                grid->reapply_changes(step.cells);
            else
                grid->revert_changes(step.cells);
            return true;
        }
        case Step::Kind::Mesh: {
            mesh::Mesh* m = mesh_for ? mesh_for(step.layer) : nullptr;
            if (!m) return false;
            // VertexDeltas refuses a mesh of a different vertex count, which is
            // a record paired with the wrong mesh. Propagated rather than
            // ignored: the step stays on the stack and the caller is told.
            return forward ? step.deltas.apply(*m) : step.deltas.revert(*m);
        }
        case Step::Kind::Barrier:
            return false;
    }
    return false;
}

bool History::undo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
                   math::Aabb* out_bound) {
    if (!enabled_ || steps_.empty()) return false;
    // A barrier is not reversible and must not be silently consumed: it is the
    // horizon a host draws. Report nothing to undo and leave it in place.
    if (!steps_.back().reversible()) return false;
    Step step = std::move(steps_.back());
    if (!apply_step(step, /*forward=*/false, doc, grid_for, mesh_for, out_bound)) {
        steps_.back() = std::move(step);  // unchanged; the caller is told
        return false;
    }
    steps_.pop_back();
    redo_.push_back(std::move(step));
    return true;
}

bool History::redo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
                   math::Aabb* out_bound) {
    if (!enabled_ || redo_.empty()) return false;
    Step step = std::move(redo_.back());
    if (!apply_step(step, /*forward=*/true, doc, grid_for, mesh_for, out_bound)) {
        redo_.back() = std::move(step);
        return false;
    }
    redo_.pop_back();
    steps_.push_back(std::move(step));
    return true;
}

std::size_t History::undo_depth() const {
    // Steps that will actually reverse something, and only as far back as the
    // nearest barrier — past it nothing is reversible, so counting further
    // would promise undos a host cannot perform.
    std::size_t n = 0;
    for (std::size_t i = steps_.size(); i > 0; --i) {
        if (!steps_[i - 1].reversible()) break;
        ++n;
    }
    return n;
}

std::size_t History::redo_depth() const {
    std::size_t n = 0;
    for (std::size_t i = redo_.size(); i > 0; --i) {
        if (!redo_[i - 1].reversible()) break;
        ++n;
    }
    return n;
}

const std::string& History::next_barrier() const {
    const std::size_t reversible = undo_depth();
    if (reversible >= steps_.size()) return kNoBarrier;  // nothing beneath
    return steps_[steps_.size() - reversible - 1].barrier;
}

void History::clear() {
    steps_.clear();
    redo_.clear();
    open_cells_.clear();
    voxel_open_ = false;
    open_layer_ = 0;
}

}  // namespace session
}  // namespace clay
