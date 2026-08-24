#include "clay/session/history.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace clay {
namespace session {

namespace {
const std::string kNoBarrier;
}  // namespace

void History::push(Step step) {
    steps_.push_back(std::move(step));
    enforce_budget();
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
    // Journaled per COMMAND, whether or not it made a new step: a coalesced
    // stroke is many commands and one step, and replay must re-perform all of
    // them to reproduce the entry.
    if (ok) {
        JournalEvent e;
        e.kind = JournalEvent::Kind::Command;
        e.command = cmd;
        journal_.push_back(std::move(e));
    }
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
    if (enabled_) {
        JournalEvent e;
        e.kind = JournalEvent::Kind::GroupBegin;
        journal_.push_back(std::move(e));
    }
    commands_.begin_group();
}

void History::end_group() {
    commands_.end_group();
    grouping_ = false;
    if (enabled_) {
        JournalEvent e;
        e.kind = JournalEvent::Kind::GroupEnd;
        journal_.push_back(std::move(e));
    }
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
    JournalEvent e;
    e.kind = JournalEvent::Kind::Voxel;
    e.layer = step.layer;
    e.cells = step.cells;
    journal_.push_back(std::move(e));
    push(std::move(step));
}

bool History::begin_group_step(voxel::GroupField& groups) {
    if (!enabled_) return false;
    if (group_open_) return false;  // a step is one edit
    group_open_ = true;
    // EAGER, unlike the mask's lazy snapshot: a group field has no touch() hook
    // to hang a first-write snapshot on — that is the whole reason this is a
    // third mechanism — and it is kilobytes rather than megabytes, so taking it
    // up front costs nothing worth a hook.
    group_snapshot_ = groups.serialize();
    return true;
}

void History::end_group_step(voxel::GroupField& groups) {
    if (!group_open_) return;
    group_open_ = false;
    std::vector<std::uint8_t> after = groups.serialize();
    // An edit that changed nothing is not a step. Isolating the group already
    // isolated is an ordinary thing to do, and it must not put an undo that
    // does nothing into the menu.
    if (after == group_snapshot_) {
        group_snapshot_.clear();
        return;
    }
    Step step;
    step.kind = Step::Kind::SurfaceGroup;
    step.group_before = std::move(group_snapshot_);
    step.group_after = after;
    group_snapshot_.clear();
    JournalEvent e;
    e.kind = JournalEvent::Kind::SurfaceGroup;
    e.group_after = std::move(after);
    journal_.push_back(std::move(e));
    push(std::move(step));
}

bool History::begin_mask_step(scene::LayerId layer, voxel::MaskField& mask) {
    if (!enabled_) return false;
    if (mask_open_) return false;  // a step is one edit
    if (!mask.begin_step()) return false;
    open_mask_layer_ = layer;
    mask_open_ = true;
    return true;
}

void History::end_mask_step(voxel::MaskField& mask) {
    if (!mask_open_) return;
    mask_open_ = false;
    std::vector<voxel::MaskField::MaskChange> cells = mask.end_step();
    // A step that changed no cell is dropped, exactly as a voxel one is: a
    // paint that landed on cells already at that value is ordinary, and an
    // undo that does nothing is what this mechanism exists to remove.
    if (cells.empty()) return;
    Step step;
    step.kind = Step::Kind::Mask;
    step.layer = open_mask_layer_;
    step.mask_cells = std::move(cells);
    JournalEvent e;
    e.kind = JournalEvent::Kind::Mask;
    e.layer = step.layer;
    e.mask_cells = step.mask_cells;
    journal_.push_back(std::move(e));
    push(std::move(step));
}

void History::record_mesh_step(scene::LayerId layer, mesh::VertexDeltas deltas) {
    if (!enabled_ || deltas.empty()) return;
    Step step;
    step.kind = Step::Kind::Mesh;
    step.layer = layer;
    step.deltas = std::move(deltas);
    JournalEvent e;
    e.kind = JournalEvent::Kind::Mesh;
    e.layer = step.layer;
    e.deltas = step.deltas;
    journal_.push_back(std::move(e));
    push(std::move(step));
}

void History::record_barrier(std::string what) {
    if (!enabled_) return;
    Step step;
    step.kind = Step::Kind::Barrier;
    step.barrier = what;
    JournalEvent e;
    e.kind = JournalEvent::Kind::Barrier;
    e.barrier = std::move(what);
    journal_.push_back(std::move(e));
    push(std::move(step));
}

bool History::apply_step(const Step& step, bool forward, scene::Document& doc,
                         const GridFor& grid_for, const MeshFor& mesh_for,
                         math::Aabb* out_bound, const MaskFor& mask_for) {
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
        case Step::Kind::Mask: {
            voxel::MaskField* mask = mask_for ? mask_for(step.layer) : nullptr;
            // Refused rather than skipped, for the same reason a missing grid
            // is: skipping would take the step off the stack and leave the
            // next undo reversing something older than the user asked for.
            if (!mask) return false;
            if (forward)
                mask->reapply_changes(step.mask_cells);
            else
                mask->revert_changes(step.mask_cells);
            return true;
        }
        case Step::Kind::SurfaceGroup: {
            voxel::GroupField* groups = groups_for_ ? groups_for_() : nullptr;
            // Refused rather than skipped, for the reason a missing grid is.
            if (!groups) return false;
            const std::vector<std::uint8_t>& want = forward ? step.group_after : step.group_before;
            std::optional<voxel::GroupField> restored =
                voxel::GroupField::deserialize(want.data(), want.size());
            if (!restored) return false;
            *groups = std::move(*restored);
            return true;
        }
        case Step::Kind::Barrier:
            return false;
    }
    return false;
}

bool History::undo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
                   math::Aabb* out_bound, const MaskFor& mask_for) {
    if (!enabled_ || steps_.empty()) return false;
    // A barrier is not reversible and must not be silently consumed: it is the
    // horizon a host draws. Report nothing to undo and leave it in place.
    if (!steps_.back().reversible()) return false;
    Step step = std::move(steps_.back());
    if (!apply_step(step, /*forward=*/false, doc, grid_for, mesh_for, out_bound, mask_for)) {
        steps_.back() = std::move(step);  // unchanged; the caller is told
        return false;
    }
    steps_.pop_back();
    redo_.push_back(std::move(step));
    {
        JournalEvent e;
        e.kind = JournalEvent::Kind::Undo;
        journal_.push_back(std::move(e));
    }
    return true;
}

bool History::redo(scene::Document& doc, const GridFor& grid_for, const MeshFor& mesh_for,
                   math::Aabb* out_bound, const MaskFor& mask_for) {
    if (!enabled_ || redo_.empty()) return false;
    Step step = std::move(redo_.back());
    if (!apply_step(step, /*forward=*/true, doc, grid_for, mesh_for, out_bound, mask_for)) {
        redo_.back() = std::move(step);
        return false;
    }
    redo_.pop_back();
    steps_.push_back(std::move(step));
    {
        JournalEvent e;
        e.kind = JournalEvent::Kind::Redo;
        journal_.push_back(std::move(e));
    }
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

// -- the journal (survive-a-crash) -------------------------------------------
//
// Layout, little-endian:
//
//   u32 magic 'CJRN'   u16 version   u16 reserved   u32 event_count
//   per event:  u8 kind   u8 step_kind   u32 layer   u32 payload_bytes   payload
//
// Versioned so a build that does not understand a journal REFUSES it. A
// recovery that silently drops what it could not read is the failure this
// whole feature exists to prevent, and it is worse than no recovery because
// the user cannot see what is missing.
namespace {

constexpr std::uint32_t kJournalMagic = 0x4E524A43u;  // 'CJRN'
constexpr std::uint16_t kJournalVersion = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_bytes(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& b) {
    put_u32(out, static_cast<std::uint32_t>(b.size()));
    out.insert(out.end(), b.begin(), b.end());
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    bool ok = true;

    std::uint8_t u8() {
        if (at + 1 > size) { ok = false; return 0; }
        return data[at++];
    }
    std::uint32_t u32() {
        if (at + 4 > size) { ok = false; return 0; }
        const std::uint32_t v = static_cast<std::uint32_t>(data[at]) |
                                (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                                (static_cast<std::uint32_t>(data[at + 2]) << 16) |
                                (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return v;
    }
    const std::uint8_t* block(std::uint32_t n) {
        if (at + n > size) { ok = false; return nullptr; }
        const std::uint8_t* p = data + at;
        at += n;
        return p;
    }
};

// A mask step is a run of PODs too, with the value already quantized.
std::vector<std::uint8_t> encode_mask_cells(
    const std::vector<voxel::MaskField::MaskChange>& cells) {
    std::vector<std::uint8_t> out;
    out.reserve(cells.size() * 14);
    for (const voxel::MaskField::MaskChange& c : cells) {
        put_u32(out, static_cast<std::uint32_t>(c.cell.x));
        put_u32(out, static_cast<std::uint32_t>(c.cell.y));
        put_u32(out, static_cast<std::uint32_t>(c.cell.z));
        out.push_back(c.before);
        out.push_back(c.after);
    }
    return out;
}

bool decode_mask_cells(const std::uint8_t* data, std::size_t size,
                       std::vector<voxel::MaskField::MaskChange>* out) {
    if (size % 14 != 0) return false;
    out->clear();
    out->reserve(size / 14);
    Reader r{data, size};
    while (r.at < size && r.ok) {
        voxel::MaskField::MaskChange c;
        c.cell.x = static_cast<std::int32_t>(r.u32());
        c.cell.y = static_cast<std::int32_t>(r.u32());
        c.cell.z = static_cast<std::int32_t>(r.u32());
        c.before = r.u8();
        c.after = r.u8();
        out->push_back(c);
    }
    return r.ok;
}

// A voxel step is a run of PODs, so its encoding is the run.
std::vector<std::uint8_t> encode_cells(const std::vector<voxel::VoxelGrid::SculptChange>& cells) {
    std::vector<std::uint8_t> out;
    out.reserve(cells.size() * 14);
    for (const voxel::VoxelGrid::SculptChange& c : cells) {
        put_u32(out, static_cast<std::uint32_t>(c.cell.x));
        put_u32(out, static_cast<std::uint32_t>(c.cell.y));
        put_u32(out, static_cast<std::uint32_t>(c.cell.z));
        out.push_back(c.before);
        out.push_back(c.after);
    }
    return out;
}

bool decode_cells(const std::uint8_t* data, std::size_t size,
                  std::vector<voxel::VoxelGrid::SculptChange>* out) {
    if (size % 14 != 0) return false;
    out->clear();
    out->reserve(size / 14);
    Reader r{data, size};
    while (r.at < size && r.ok) {
        voxel::VoxelGrid::SculptChange c;
        c.cell.x = static_cast<std::int32_t>(r.u32());
        c.cell.y = static_cast<std::int32_t>(r.u32());
        c.cell.z = static_cast<std::int32_t>(r.u32());
        c.before = r.u8();
        c.after = r.u8();
        out->push_back(c);
    }
    return r.ok;
}

}  // namespace

std::vector<std::uint8_t> History::journal_since(std::size_t from, std::size_t* out_now_at) const {
    const std::size_t next = journal_next();
    if (out_now_at) *out_now_at = next;

    std::vector<std::uint8_t> out;
    put_u32(out, kJournalMagic);
    out.push_back(static_cast<std::uint8_t>(kJournalVersion));
    out.push_back(static_cast<std::uint8_t>(kJournalVersion >> 8));
    out.push_back(0);
    out.push_back(0);

    // Below the base is a host asking for events that were trimmed. Answering
    // with what remains would silently hand back a shorter history than was
    // asked for, so it yields nothing and the caller sees journal_first() moved.
    if (from < journal_base_ || from >= next) {
        put_u32(out, 0);
        return out;
    }
    const std::size_t begin = from - journal_base_;
    put_u32(out, static_cast<std::uint32_t>(journal_.size() - begin));

    for (std::size_t i = begin; i < journal_.size(); ++i) {
        const JournalEvent& e = journal_[i];
        out.push_back(static_cast<std::uint8_t>(e.kind));
        put_u32(out, e.layer);
        switch (e.kind) {
            case JournalEvent::Kind::Command:
                put_bytes(out, scene::serialize(e.command));
                break;
            case JournalEvent::Kind::Voxel:
                put_bytes(out, encode_cells(e.cells));
                break;
            case JournalEvent::Kind::Mesh:
                put_bytes(out, e.deltas.encode());
                break;
            case JournalEvent::Kind::Mask:
                put_bytes(out, encode_mask_cells(e.mask_cells));
                break;
            case JournalEvent::Kind::SurfaceGroup:
                // The serialised field itself: it is already a compact,
                // deterministic blob, so a second encoding would buy nothing.
                put_bytes(out, e.group_after);
                break;
            case JournalEvent::Kind::Barrier:
                put_bytes(out, std::vector<std::uint8_t>(e.barrier.begin(), e.barrier.end()));
                break;
            case JournalEvent::Kind::GroupBegin:
            case JournalEvent::Kind::GroupEnd:
            case JournalEvent::Kind::Undo:
            case JournalEvent::Kind::Redo:
                put_u32(out, 0);
                break;
        }
    }
    return out;
}

void History::trim_journal(std::size_t upto) {
    if (upto <= journal_base_) return;
    const std::size_t drop = std::min(upto - journal_base_, journal_.size());
    journal_.erase(journal_.begin(), journal_.begin() + static_cast<std::ptrdiff_t>(drop));
    journal_base_ += drop;
}

bool History::replay(const std::uint8_t* data, std::size_t size, scene::Document& doc,
                     const GridFor& grid_for, const MeshFor& mesh_for, ReplayResult* out,
                     const MaskFor& mask_for) {
    ReplayResult result;
    if (out) *out = result;
    if (!data || size == 0) return false;
    Reader r{data, size};
    if (r.u32() != kJournalMagic) return false;
    const std::uint16_t version =
        static_cast<std::uint16_t>(r.u8() | (static_cast<std::uint16_t>(r.u8()) << 8));
    r.u8();
    r.u8();
    // Refused rather than partially interpreted. A recovery that silently drops
    // what it could not read is worse than none: the user cannot see the gap.
    if (!r.ok || version != kJournalVersion) return false;
    const std::uint32_t count = r.u32();
    if (!r.ok) return false;

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t kind = r.u8();
        const scene::LayerId layer = r.u32();
        const std::uint32_t payload = r.u32();
        const std::uint8_t* body = payload ? r.block(payload) : nullptr;
        if (!r.ok) {
            if (out) *out = result;
            return false;  // truncated: what was applied stands, the rest is refused
        }

        switch (static_cast<JournalEvent::Kind>(kind)) {
            case JournalEvent::Kind::Command: {
                std::optional<scene::Command> cmd = scene::deserialize(body, payload);
                // Replayed through perform(), which is what makes coalescing and
                // grouping reproduce themselves instead of being re-derived.
                if (!cmd || !perform(doc, *cmd)) {
                    if (out) *out = result;
                    return false;
                }
                break;
            }
            case JournalEvent::Kind::GroupBegin:
                begin_group();
                break;
            case JournalEvent::Kind::GroupEnd:
                end_group();
                break;
            case JournalEvent::Kind::Voxel: {
                voxel::VoxelGrid* grid = grid_for ? grid_for(layer) : nullptr;
                std::vector<voxel::VoxelGrid::SculptChange> cells;
                if (!grid || !decode_cells(body, payload, &cells)) {
                    if (out) *out = result;
                    return false;
                }
                grid->reapply_changes(cells);
                Step step;
                step.kind = Step::Kind::Voxel;
                step.layer = layer;
                step.cells = std::move(cells);
                JournalEvent e;
                e.kind = JournalEvent::Kind::Voxel;
                e.layer = layer;
                e.cells = step.cells;
                journal_.push_back(std::move(e));
                push(std::move(step));
                break;
            }
            case JournalEvent::Kind::Mesh: {
                mesh::Mesh* m = mesh_for ? mesh_for(layer) : nullptr;
                mesh::VertexDeltas deltas;
                if (!m || !mesh::VertexDeltas::decode(body, payload, &deltas) ||
                    !deltas.apply(*m)) {
                    if (out) *out = result;
                    return false;
                }
                Step step;
                step.kind = Step::Kind::Mesh;
                step.layer = layer;
                step.deltas = deltas;
                JournalEvent e;
                e.kind = JournalEvent::Kind::Mesh;
                e.layer = layer;
                e.deltas = std::move(deltas);
                journal_.push_back(std::move(e));
                push(std::move(step));
                break;
            }
            case JournalEvent::Kind::Mask: {
                voxel::MaskField* mask = mask_for ? mask_for(layer) : nullptr;
                std::vector<voxel::MaskField::MaskChange> cells;
                if (!mask || !decode_mask_cells(body, payload, &cells)) {
                    if (out) *out = result;
                    return false;
                }
                mask->reapply_changes(cells);
                Step step;
                step.kind = Step::Kind::Mask;
                step.layer = layer;
                step.mask_cells = std::move(cells);
                JournalEvent e;
                e.kind = JournalEvent::Kind::Mask;
                e.layer = layer;
                e.mask_cells = step.mask_cells;
                journal_.push_back(std::move(e));
                push(std::move(step));
                break;
            }
            case JournalEvent::Kind::SurfaceGroup: {
                voxel::GroupField* groups = groups_for_ ? groups_for_() : nullptr;
                std::optional<voxel::GroupField> restored =
                    voxel::GroupField::deserialize(body, payload);
                if (!groups || !restored) {
                    if (out) *out = result;
                    return false;
                }
                // The step's BEFORE side is the field as it stands right now,
                // captured before overwriting it: a replay walks forward from a
                // snapshot, so "before" is whatever the previous event left,
                // and taking it here is what keeps the reconstructed step list
                // undoable rather than one-way.
                Step step;
                step.kind = Step::Kind::SurfaceGroup;
                step.group_before = groups->serialize();
                step.group_after.assign(body, body + payload);
                *groups = std::move(*restored);
                JournalEvent e;
                e.kind = JournalEvent::Kind::SurfaceGroup;
                e.group_after = step.group_after;
                journal_.push_back(std::move(e));
                push(std::move(step));
                break;
            }
            case JournalEvent::Kind::Barrier:
                // STOPS rather than skips. Continuing past an operation it
                // cannot reproduce would hand back a document quietly missing
                // that operation's effect, and the user could not see the loss.
                result.stopped_at_barrier = true;
                result.barrier.assign(reinterpret_cast<const char*>(body), payload);
                if (out) *out = result;
                return true;
            case JournalEvent::Kind::Undo:
                if (!undo(doc, grid_for, mesh_for, nullptr, mask_for)) {
                    if (out) *out = result;
                    return false;
                }
                break;
            case JournalEvent::Kind::Redo:
                if (!redo(doc, grid_for, mesh_for, nullptr, mask_for)) {
                    if (out) *out = result;
                    return false;
                }
                break;
        }
        ++result.applied;
    }
    if (out) *out = result;
    return true;
}

// -- what it costs (add-history-budget) --------------------------------------

std::size_t History::step_bytes(const Step& s) {
    // What the step OWNS, not sizeof — the entries that matter are the ones
    // holding heap payloads, and sizeof would report them all the same.
    std::size_t n = sizeof(Step);
    n += s.cells.capacity() * sizeof(voxel::VoxelGrid::SculptChange);
    n += s.mask_cells.capacity() * sizeof(voxel::MaskField::MaskChange);
    n += s.barrier.capacity();
    n += s.deltas.bytes();
    // A SurfaceGroup step holds two whole serialised fields, which makes this
    // the term that matters for it rather than a rounding error — the same
    // omission roll-up-document-memory found six of in node_bytes.
    n += s.group_before.capacity() + s.group_after.capacity();
    return n;
}

std::size_t History::event_bytes(const JournalEvent& e) {
    std::size_t n = sizeof(JournalEvent);
    n += e.cells.capacity() * sizeof(voxel::VoxelGrid::SculptChange);
    n += e.mask_cells.capacity() * sizeof(voxel::MaskField::MaskChange);
    n += e.barrier.capacity();
    n += e.deltas.bytes();
    n += e.group_after.capacity();
    n += scene::command_bytes(e.command);
    return n;
}

History::Bytes History::bytes() const {
    Bytes out;
    out.undo_steps = steps_.size();
    out.redo_steps = redo_.size();
    out.journal_events = journal_.size();
    out.dropped_steps = dropped_steps_;
    for (const Step& s : steps_) out.undo += step_bytes(s);
    for (const Step& s : redo_) out.redo += step_bytes(s);
    for (const JournalEvent& e : journal_) out.journal += event_bytes(e);
    // The command stack under the Scene steps, which the steps themselves do
    // not carry — and which is where a session of DELETES hides its cost.
    out.undo += commands_.undo_bytes();
    out.redo += commands_.redo_bytes();
    out.total = out.undo + out.redo + out.journal;
    return out;
}

void History::set_budget(std::size_t bytes) {
    budget_ = bytes;
    enforce_budget();
}

void History::enforce_budget() {
    if (budget_ == 0) return;  // unbounded: exactly today's behaviour
    // Redo goes first. It is transient — the next edit discards it anyway — so
    // spending the budget on it before the undo the user can actually reach
    // would be the wrong trade.
    while (!redo_.empty()) {
        Bytes b = bytes();
        if (b.undo + b.redo <= budget_) return;
        redo_.erase(redo_.begin());
    }
    // Then the oldest undo steps. NEVER the newest: a budget that could make
    // the next undo fail would be worse than no budget, because a host cannot
    // tell that from a bug.
    while (steps_.size() > 1) {
        Bytes b = bytes();
        if (b.undo + b.redo <= budget_) return;
        steps_.erase(steps_.begin());
        ++dropped_steps_;
    }
}

void History::trim_to(std::size_t bytes_limit) {
    while (!redo_.empty()) {
        const Bytes b = bytes();
        if (b.undo + b.redo <= bytes_limit) return;
        redo_.erase(redo_.begin());
    }
    while (steps_.size() > 1) {
        const Bytes b = bytes();
        if (b.undo + b.redo <= bytes_limit) return;
        steps_.erase(steps_.begin());
        ++dropped_steps_;
    }
}

void History::clear() {
    steps_.clear();
    redo_.clear();
    journal_.clear();
    journal_base_ = 0;
    dropped_steps_ = 0;
    open_cells_.clear();
    voxel_open_ = false;
    mask_open_ = false;
    open_layer_ = 0;
    open_mask_layer_ = 0;
}

}  // namespace session
}  // namespace clay
