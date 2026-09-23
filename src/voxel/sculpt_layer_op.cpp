// A sculpt-layer operation as an undo step (unify-the-undo-history 3.3, 3.4).
//
// The operations themselves live beside the rest of the stack in grid.cpp;
// this file holds what makes one REPLAYABLE — the apply in either direction,
// the data a removal or a merge-down has to keep, and the journal encoding.

#include <cstring>
#include <utility>

#include "clay/bytes.h"
#include "clay/voxel/grid.h"

namespace clay {
namespace voxel {

VoxelGrid::SculptLayerData VoxelGrid::to_data(SculptLayerRecord rec) {
    SculptLayerData d;
    d.name = std::move(rec.name);
    d.changes = std::move(rec.changes);
    d.strength = rec.strength;
    d.visible = rec.visible;
    d.seed = rec.seed;
    return d;
}

VoxelGrid::SculptLayerRecord VoxelGrid::from_data(SculptLayerData data) {
    SculptLayerRecord rec;
    rec.name = std::move(data.name);
    rec.changes = std::move(data.changes);
    rec.strength = data.strength;
    rec.visible = data.visible;
    rec.seed = data.seed;
    // The lookup is derived state: one entry per cell, at the position of the
    // change that cell owns. A record never holds a cell twice.
    rec.index.reserve(rec.changes.size());
    for (std::size_t i = 0; i < rec.changes.size(); ++i) rec.index.emplace(rec.changes[i].cell, i);
    return rec;
}

// The inverse of fold_down: truncate the lower layer to what it held, put back
// the `after` values the fold overwrote, and reinsert the upper layer.
void VoxelGrid::unfold_down(const SculptLayerOp& op) {
    SculptLayerRecord& lower = sculpt_layers_[op.layer - 1];
    for (std::size_t i = op.lower_count; i < lower.changes.size(); ++i)
        lower.index.erase(lower.changes[i].cell);
    lower.changes.resize(op.lower_count);
    for (std::size_t k = op.lower_afters.size(); k > 0; --k) {
        const auto& [at, after] = op.lower_afters[k - 1];
        lower.changes[at].after = after;
    }
    sculpt_layers_.insert(sculpt_layers_.begin() + static_cast<std::ptrdiff_t>(op.layer),
                          from_data(op.held));
}

namespace {

// Whether the stack has the shape `op` names, in the direction it is about to
// run. Checked before anything is written, so a refusal leaves the grid as it
// was rather than half-replayed.
bool op_fits(const VoxelGrid::SculptLayerOp& op, bool forward, std::size_t count,
             std::size_t lower_size) {
    using Kind = VoxelGrid::SculptLayerOp::Kind;
    switch (op.kind) {
        case Kind::Strength:
        case Kind::Visible:
            return op.layer < count;
        case Kind::Move:
            return op.layer < count && op.to < count;
        case Kind::Remove:
            // Undoing a removal reinserts, so the slot may be one past the end.
            return forward ? op.layer < count : op.layer <= count;
        case Kind::Merge: {
            if (op.layer == 0) return false;
            if (forward) return op.layer < count;
            if (op.layer > count) return false;
            if (lower_size < op.lower_count) return false;
            for (const auto& entry : op.lower_afters)
                if (entry.first >= op.lower_count) return false;
            return true;
        }
        case Kind::None:
            return false;
    }
    return false;
}

}  // namespace

bool VoxelGrid::apply_sculpt_layer_op(const SculptLayerOp& op, bool forward) {
    const std::size_t count = sculpt_layers_.size();
    const std::size_t lower_size =
        (op.layer >= 1 && op.layer - 1 < count) ? sculpt_layers_[op.layer - 1].changes.size() : 0;
    if (!op_fits(op, forward, count, lower_size)) return false;
    // Undo restores the cells FIRST, while the stack still has the shape the
    // operation left it in, and then the property; redo runs the other way.
    // Neither order matters to the cells — a replay writes by coordinate — but
    // keeping them mirror images keeps the two directions readable as one.
    if (!forward) revert_changes(op.cells);
    switch (op.kind) {
        case SculptLayerOp::Kind::Strength:
            sculpt_layers_[op.layer].strength = forward ? op.strength_after : op.strength_before;
            break;
        case SculptLayerOp::Kind::Visible:
            sculpt_layers_[op.layer].visible = forward ? op.visible_after : op.visible_before;
            break;
        case SculptLayerOp::Kind::Move:
            if (forward)
                move_record(op.layer, op.to);
            else
                move_record(op.to, op.layer);
            break;
        case SculptLayerOp::Kind::Remove:
            if (forward)
                sculpt_layers_.erase(sculpt_layers_.begin() + static_cast<std::ptrdiff_t>(op.layer));
            else
                sculpt_layers_.insert(sculpt_layers_.begin() + static_cast<std::ptrdiff_t>(op.layer),
                                      from_data(op.held));
            break;
        case SculptLayerOp::Kind::Merge:
            if (forward)
                fold_down(op.layer, nullptr);
            else
                unfold_down(op);
            break;
        case SculptLayerOp::Kind::None:
            break;
    }
    if (forward) reapply_changes(op.cells);
    return true;
}

std::size_t VoxelGrid::SculptLayerOp::bytes() const {
    return sizeof(SculptLayerOp) + held.name.capacity() + vector_bytes(held.changes) +
           vector_bytes(lower_afters) + vector_bytes(cells);
}

// -- the journal encoding ----------------------------------------------------
//
// Little-endian, and private to a session: its only reader is decode below, so
// it carries a version byte and no forward-compatibility promise.
//
//   u8 version   u8 kind   u32 layer   u32 to
//   f32 strength_before   f32 strength_after   u8 visible_before   u8 visible_after
//   held:  u32 name_len  name  f32 strength  u8 visible  u32 seed  changes
//   u32 lower_count   u32 n  n x (u32 index, u8 after)
//   cells
// where `changes` and `cells` are  u32 n  n x (i32 x, i32 y, i32 z, u8 before, u8 after).
namespace {

constexpr std::uint8_t kOpVersion = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}
void put_changes(std::vector<std::uint8_t>& out,
                 const std::vector<VoxelGrid::SculptChange>& changes) {
    put_u32(out, static_cast<std::uint32_t>(changes.size()));
    for (const VoxelGrid::SculptChange& c : changes) {
        put_u32(out, static_cast<std::uint32_t>(c.cell.x));
        put_u32(out, static_cast<std::uint32_t>(c.cell.y));
        put_u32(out, static_cast<std::uint32_t>(c.cell.z));
        out.push_back(c.before);
        out.push_back(c.after);
    }
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;
    bool ok = true;

    bool need(std::size_t n) {
        if (!ok || size - pos < n) ok = false;
        return ok;
    }
    std::uint8_t u8() {
        if (!need(1)) return 0;
        return data[pos++];
    }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[pos + i]) << (8 * i);
        pos += 4;
        return v;
    }
    float f32() {
        const std::uint32_t bits = u32();
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    // A count is checked against the bytes left BEFORE anything is reserved,
    // so a corrupt count cannot ask for gigabytes.
    bool count(std::size_t per, std::uint32_t* out) {
        *out = u32();
        return ok && need(static_cast<std::size_t>(*out) * per);
    }
    bool changes(std::vector<VoxelGrid::SculptChange>* out) {
        std::uint32_t n = 0;
        if (!count(14, &n)) return false;
        out->resize(n);
        for (VoxelGrid::SculptChange& c : *out) {
            c.cell.x = static_cast<std::int32_t>(u32());
            c.cell.y = static_cast<std::int32_t>(u32());
            c.cell.z = static_cast<std::int32_t>(u32());
            c.before = u8();
            c.after = u8();
        }
        return ok;
    }
};

}  // namespace

std::vector<std::uint8_t> VoxelGrid::SculptLayerOp::encode() const {
    std::vector<std::uint8_t> out;
    out.push_back(kOpVersion);
    out.push_back(static_cast<std::uint8_t>(kind));
    put_u32(out, static_cast<std::uint32_t>(layer));
    put_u32(out, static_cast<std::uint32_t>(to));
    put_f32(out, strength_before);
    put_f32(out, strength_after);
    out.push_back(visible_before ? 1 : 0);
    out.push_back(visible_after ? 1 : 0);
    put_u32(out, static_cast<std::uint32_t>(held.name.size()));
    out.insert(out.end(), held.name.begin(), held.name.end());
    put_f32(out, held.strength);
    out.push_back(held.visible ? 1 : 0);
    put_u32(out, held.seed);
    put_changes(out, held.changes);
    put_u32(out, static_cast<std::uint32_t>(lower_count));
    put_u32(out, static_cast<std::uint32_t>(lower_afters.size()));
    for (const auto& [at, after] : lower_afters) {
        put_u32(out, at);
        out.push_back(after);
    }
    put_changes(out, cells);
    return out;
}

bool VoxelGrid::SculptLayerOp::decode(const std::uint8_t* data, std::size_t size,
                                      SculptLayerOp* out) {
    if (!data || !out) return false;
    Reader r{data, size};
    if (r.u8() != kOpVersion) return false;
    SculptLayerOp op;
    const std::uint8_t kind = r.u8();
    if (kind == 0 || kind > static_cast<std::uint8_t>(Kind::Merge)) return false;
    op.kind = static_cast<Kind>(kind);
    op.layer = r.u32();
    op.to = r.u32();
    op.strength_before = r.f32();
    op.strength_after = r.f32();
    op.visible_before = r.u8() != 0;
    op.visible_after = r.u8() != 0;
    std::uint32_t name_len = 0;
    if (!r.count(1, &name_len)) return false;
    op.held.name.assign(reinterpret_cast<const char*>(data + r.pos), name_len);
    r.pos += name_len;
    op.held.strength = r.f32();
    op.held.visible = r.u8() != 0;
    op.held.seed = r.u32();
    if (!r.changes(&op.held.changes)) return false;
    op.lower_count = r.u32();
    std::uint32_t afters = 0;
    if (!r.count(5, &afters)) return false;
    op.lower_afters.resize(afters);
    for (auto& [at, after] : op.lower_afters) {
        at = r.u32();
        after = r.u8();
    }
    if (!r.changes(&op.cells)) return false;
    // Trailing bytes are a record this build did not write.
    if (!r.ok || r.pos != size) return false;
    *out = std::move(op);
    return true;
}

}  // namespace voxel
}  // namespace clay
