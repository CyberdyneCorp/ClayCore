// COMPOSITION: turning a stack of layers into the one field evaluation reads,
// and the surface-level operations that have to keep the two in step
// (mesh-sculpt-layers spec, add-mesh-sculpt-layers).
//
//     E(n) = B(n) + Σ sᵢ · mᵢ(v) · Lᵢ(n, v)
//
// SPLIT FROM `sculpt_layer.cpp` for the reason `multires_eval.cpp` is split
// from `multires.cpp`: that file is about a structure's life and this one is a
// loop a stroke runs sixty times a second.
//
// WHY BLOCKS ARE THE UNIT, and it is the whole of both scale gates. Every
// layer's `DetailField`, every layer's `SparseWeightField` and the level's
// composed field share the block size, so block `b` names the same 1024
// vertices in all of them. Recomposing a block therefore asks each layer one
// O(1) question — "do you store this block?" — and a layer that does not reach
// there is a miss rather than a scan. Two consequences fall out as ARITHMETIC
// rather than as optimisations a later edit could lose:
//
//   * a strength change dirties the blocks that layer has ALLOCATED, so it
//     costs the layer's coverage and not the surface (task 5.4);
//   * a stamp writes one layer's blocks, so recomposition never visits an
//     unrelated block and never sums the stack over one (task 5.5).
//
// Prefix checkpoints stay POSSIBLE without being built: a checkpoint is a
// synthetic layer over a contiguous range of the stack with its own composition
// revision, which these keys already admit. Whether one is needed is the
// benchmark's measurement to answer, not this file's.

#include <algorithm>

#include "multires_internal.h"

namespace clay {
namespace mesh {

const DetailField& effective_detail(const MultiresLevel& level) {
    return level.composed ? *level.composed : level.detail;
}

void sync_stack_levels(MultiresSurface::State& s) {
    std::vector<std::uint32_t> counts(s.levels.size());
    for (std::size_t l = 0; l < s.levels.size(); ++l)
        counts[l] = s.levels[l].topology.vertex_count;
    s.stack.set_level_sizes(counts);
}

namespace {

// The layers that actually reach one block, resolved once for the whole block
// rather than per vertex. A layer at zero effective strength is dropped here
// rather than multiplied by zero, so a stack whose layers are all hidden
// composes to EXACTLY the base detail — bit for bit, which is what makes
// "invisible contributes nothing" a bit-comparison rather than a tolerance.
struct BlockContributor {
    const DetailField* detail = nullptr;
    const SparseWeightField* mask = nullptr;
    float factor = 0.0f;
};

void gather_contributors(const SculptLayerStack& stack, std::uint32_t level, std::uint32_t block,
                         std::vector<BlockContributor>* out) {
    out->clear();
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const SculptLayer* layer = stack.at(i);
        const float factor = layer->composition_factor();
        if (factor == 0.0f) continue;
        if (level >= layer->detail.size()) continue;
        const DetailField& field = layer->detail[level];
        if (!field.block_stored(block)) continue;
        BlockContributor c;
        c.detail = &field;
        c.mask = &layer->mask[level];
        c.factor = factor;
        out->push_back(c);
    }
}

void recompose_block(MultiresSurface::State& s, MultiresLevel& lev, std::uint32_t level,
                     std::uint32_t block, std::vector<BlockContributor>* scratch) {
    const std::uint32_t bs = s.stack.block_size();
    const std::uint32_t begin = block * bs;
    const std::uint32_t end = std::min(begin + bs, lev.topology.vertex_count);
    if (begin >= end) return;
    gather_contributors(s.stack, level, block, scratch);
    ++s.stack.stats_mutable().blocks_recomposed;
    s.stack.stats_mutable().layer_blocks_visited += scratch->size();

    for (std::uint32_t v = begin; v < end; ++v) {
        LocalDetail value = lev.detail.get(v);
        for (const BlockContributor& c : *scratch) {
            const float w = c.factor * c.mask->get(v);
            if (w == 0.0f) continue;
            const LocalDetail d = c.detail->get(v);
            value.tangent += w * d.tangent;
            value.bitangent += w * d.bitangent;
            value.normal += w * d.normal;
        }
        // A vertex that composes back to zero writes a zero, which releases if
        // the block is already gone and clears the entry if it is not. The
        // composed field is a cache, so a stale coefficient there would be a
        // wrinkle that survives the removal of the layer that made it.
        lev.composed->set(v, value);
    }
}

void append_block_vertices(const MultiresLevel& lev, std::uint32_t block, std::uint32_t bs,
                           std::vector<std::uint32_t>* out) {
    if (!out) return;
    const std::uint32_t begin = block * bs;
    const std::uint32_t end = std::min(begin + bs, lev.topology.vertex_count);
    for (std::uint32_t v = begin; v < end; ++v) out->push_back(v);
}

// Every vertex the composed field was covering, for the one case that has to
// invalidate the level whole: the last layer leaving it.
void append_stored_vertices(const MultiresLevel& lev, std::uint32_t bs,
                            std::vector<std::uint32_t>* out) {
    if (!out || !lev.composed) return;
    const std::uint32_t stored = lev.composed->stored_block_count();
    for (std::uint32_t i = 0; i < stored; ++i)
        append_block_vertices(lev, lev.composed->stored_block_at(i), bs, out);
}

}  // namespace

bool composition_pending(const MultiresSurface::State& s) {
    if (s.stack.empty()) return false;
    if (s.stack.any_dirty()) return true;
    // A level that reaches the stack and has no composed field yet is work
    // waiting, however clean the dirty sets look — which is the state a
    // released cache leaves behind.
    for (std::uint32_t l = 0; l < static_cast<std::uint32_t>(s.levels.size()); ++l)
        if (s.stack.reaches_level(l) && !s.levels[l].composed) return true;
    return false;
}

void ensure_composed(MultiresSurface::State& s, std::uint32_t level,
                     std::vector<std::uint32_t>* out_touched) {
    if (!s.level_ok(level)) return;
    MultiresLevel& lev = s.levels[level];

    if (!s.stack.reaches_level(level)) {
        // NOTHING REACHES HERE, so there is nothing to compose and the level
        // goes back to reading its own base detail through the same call it
        // always made. Releasing rather than leaving a stale copy is what makes
        // removing the last layer restore the pre-layer bits exactly.
        if (lev.composed) {
            append_stored_vertices(lev, s.stack.block_size(), out_touched);
            lev.composed.reset();
        }
        s.stack.clear_dirty(level);
        return;
    }

    const std::uint32_t bs = s.stack.block_size();
    bool fresh = false;
    if (!lev.composed || lev.composed->vertex_count() != lev.topology.vertex_count) {
        lev.composed = std::make_unique<DetailField>();
        lev.composed->reset(lev.topology.vertex_count, bs);
        fresh = true;
    }

    std::vector<BlockContributor> scratch;
    ++s.stack.stats_mutable().compositions;
    if (fresh || s.stack.level_all_dirty(level)) {
        const std::uint32_t blocks = s.stack.level_block_count(level);
        for (std::uint32_t b = 0; b < blocks; ++b) {
            recompose_block(s, lev, level, b, &scratch);
            append_block_vertices(lev, b, bs, out_touched);
        }
    } else {
        for (std::uint32_t b : s.stack.dirty_blocks(level)) {
            recompose_block(s, lev, level, b, &scratch);
            append_block_vertices(lev, b, bs, out_touched);
        }
    }
    s.stack.clear_dirty(level);
}

// -- base deformation layers (level zero) -------------------------------------

const MultiresSurface::State::BaseRestFrames* base_rest_frames(MultiresSurface::State& s) {
    if (s.levels.empty() || !s.levels[0].composed) return nullptr;
    if (!s.base_rest) s.base_rest = std::make_unique<MultiresSurface::State::BaseRestFrames>();
    MultiresSurface::State::BaseRestFrames& r = *s.base_rest;
    if (r.valid) return &r;

    // Built WHOLE rather than partially, and that is affordable precisely
    // because it is level 0 — the cage, the smallest level in the hierarchy.
    // The partial machinery every other level needs would be a third code path
    // through `build_base_frames` to save a walk over a few thousand vertices.
    gather_class_positions(s, &r.positions);
    const LevelTopology& topology = s.levels[0].topology;
    const LevelConnectivity& conn = connectivity_of(s, 0);
    level_normals(topology, conn, r.positions, &r.normals);
    build_base_frames(topology, conn, r.positions, r.normals, nullptr, &r.frames);
    r.valid = true;
    return &r;
}

kernel::cfloat3 base_layer_offset(MultiresSurface::State& s, std::uint32_t vertex) {
    const MultiresSurface::State::BaseRestFrames* rest = base_rest_frames(s);
    if (!rest || vertex >= rest->frames.size()) return kernel::cf3(0, 0, 0);
    const LocalDetail d = s.levels[0].composed->get(vertex);
    if (d.zero()) return kernel::cf3(0, 0, 0);
    return frame_to_world(rest->frames[vertex], d.tangent, d.bitangent, d.normal);
}

// -- the surface's layer operations -------------------------------------------
//
// HERE RATHER THAN IN `multires.cpp` for one reason: baking a base deformation
// layer moves the CAGE, and the offset it moves it by is only meaningful in the
// rest frames built above. Splitting the operation from the frames it reads
// would put half of one idea in each file.
//
// Every one of these is a thin forward to the stack PLUS the bookkeeping the
// stack cannot do for itself: the per-level sizes, and the record an undo step
// needs. The stack is not exposed mutably precisely so that this pairing cannot
// be skipped.

namespace {

void record_structural(SculptLayerProperty* record, SculptLayerId id,
                       std::vector<std::uint8_t> before, const SculptLayerStack& after) {
    if (!record) return;
    record->op = SculptLayerProperty::Op::Structural;
    record->layer = id;
    record->stack_before = std::move(before);
    record->stack_after = after.encode();
}

}  // namespace

const SculptLayerStack& MultiresSurface::sculpt_layers() const {
    static const SculptLayerStack kEmpty;
    return state_ ? state_->stack : kEmpty;
}

const SculptLayerStats& MultiresSurface::sculpt_layer_stats() const {
    static const SculptLayerStats kEmpty;
    return state_ ? state_->stack.stats() : kEmpty;
}

void MultiresSurface::reset_sculpt_layer_stats() {
    if (state_) state_->stack.reset_stats();
}

std::uint64_t MultiresSurface::sculpt_layer_metadata_revision() const {
    return state_ ? state_->stack.metadata_revision() : 0;
}
std::uint64_t MultiresSurface::sculpt_layer_composition_revision() const {
    return state_ ? state_->stack.composition_revision() : 0;
}
std::uint64_t MultiresSurface::sculpt_layer_content_revision() const {
    return state_ ? state_->stack.content_revision() : 0;
}

std::uint64_t MultiresSurface::sculpt_layer_checksum() const {
    std::uint64_t h = 0xcbf29ce484222325ull;
    if (!state_) return h;
    const auto fold = [&h](std::uint64_t c) {
        for (int i = 0; i < 8; ++i) {
            h ^= (c >> (i * 8)) & 0xffull;
            h *= 0x100000001b3ull;
        }
    };
    for (std::size_t i = 0; i < state_->stack.size(); ++i) {
        const SculptLayer* layer = state_->stack.at(i);
        fold(layer->id);
        for (const DetailField& f : layer->detail) fold(f.checksum());
        for (const SparseWeightField& m : layer->mask) fold(m.checksum());
    }
    return h;
}

SculptLayerId MultiresSurface::add_sculpt_layer(std::string name, SculptLayerProperty* record) {
    if (!state_) return kNoSculptLayer;
    std::vector<std::uint8_t> before;
    if (record) before = state_->stack.encode();
    const SculptLayerId id = state_->stack.add(std::move(name));
    if (id == kNoSculptLayer) return kNoSculptLayer;
    record_structural(record, id, std::move(before), state_->stack);
    return id;
}

bool MultiresSurface::remove_sculpt_layer(SculptLayerId id, SculptLayerProperty* record) {
    if (!state_) return false;
    std::vector<std::uint8_t> before;
    if (record) before = state_->stack.encode();
    if (!state_->stack.remove(id)) return false;
    record_structural(record, id, std::move(before), state_->stack);
    return true;
}

bool MultiresSurface::move_sculpt_layer(SculptLayerId id, std::size_t index,
                                        SculptLayerProperty* record) {
    if (!state_) return false;
    std::vector<std::uint8_t> before;
    if (record) before = state_->stack.encode();
    if (!state_->stack.move_to(id, index)) return false;
    record_structural(record, id, std::move(before), state_->stack);
    return true;
}

bool MultiresSurface::merge_sculpt_layer_down(SculptLayerId id, SculptLayerProperty* record) {
    if (!state_) return false;
    std::vector<std::uint8_t> before;
    if (record) before = state_->stack.encode();
    if (!state_->stack.merge_down(id)) return false;
    record_structural(record, id, std::move(before), state_->stack);
    return true;
}

bool MultiresSurface::set_sculpt_layer_strength(SculptLayerId id, float strength,
                                                SculptLayerProperty* record) {
    if (!state_) return false;
    const SculptLayer* layer = state_->stack.find(id);
    if (!layer) return false;
    const float before = layer->strength;
    if (!state_->stack.set_strength(id, strength)) return false;
    if (record) {
        record->op = SculptLayerProperty::Op::Strength;
        record->layer = id;
        record->strength_before = before;
        // Read BACK rather than echoing the argument: the stack clamps, and a
        // record holding the unclamped value would redo to a state the stack
        // would refuse to reach.
        record->strength_after = state_->stack.find(id)->strength;
    }
    return true;
}

bool MultiresSurface::set_sculpt_layer_visible(SculptLayerId id, bool visible,
                                               SculptLayerProperty* record) {
    if (!state_) return false;
    const SculptLayer* layer = state_->stack.find(id);
    if (!layer) return false;
    const bool before = layer->visible;
    if (!state_->stack.set_visible(id, visible)) return false;
    if (record) {
        record->op = SculptLayerProperty::Op::Visible;
        record->layer = id;
        record->flag_before = before;
        record->flag_after = visible;
    }
    return true;
}

bool MultiresSurface::set_sculpt_layer_locked(SculptLayerId id, bool locked,
                                              SculptLayerProperty* record) {
    if (!state_) return false;
    const SculptLayer* layer = state_->stack.find(id);
    if (!layer) return false;
    const bool before = layer->locked;
    if (!state_->stack.set_locked(id, locked)) return false;
    if (record) {
        record->op = SculptLayerProperty::Op::Locked;
        record->layer = id;
        record->flag_before = before;
        record->flag_after = locked;
    }
    return true;
}

bool MultiresSurface::rename_sculpt_layer(SculptLayerId id, std::string name,
                                          SculptLayerProperty* record) {
    if (!state_) return false;
    const SculptLayer* layer = state_->stack.find(id);
    if (!layer) return false;
    std::string before = layer->name;
    if (!state_->stack.rename(id, name)) return false;
    if (record) {
        record->op = SculptLayerProperty::Op::Rename;
        record->layer = id;
        record->name_before = std::move(before);
        record->name_after = std::move(name);
    }
    return true;
}

bool MultiresSurface::set_active_sculpt_layer(SculptLayerId id, SculptLayerProperty* record) {
    if (!state_) return false;
    const SculptLayerId before = state_->stack.active();
    if (!state_->stack.set_active(id)) return false;
    if (record) {
        record->op = SculptLayerProperty::Op::Active;
        record->layer = id;
        record->active_before = before;
        record->active_after = id;
    }
    return true;
}

bool MultiresSurface::set_sculpt_layer_mask(SculptLayerId id, std::uint32_t level,
                                            std::uint32_t vertex, float weight) {
    return state_ && state_->stack.set_mask(id, level, vertex, weight);
}

bool MultiresSurface::set_sculpt_layer_detail(SculptLayerId id, std::uint32_t level,
                                              std::uint32_t vertex, const LocalDetail& value) {
    if (!state_) return false;
    const SculptLayer* layer = state_->stack.find(id);
    if (!layer || layer->locked) return false;
    return state_->stack.set_detail(id, level, vertex, value);
}

void MultiresSurface::hold_sculpt_layer_composition(bool held) {
    if (state_) state_->stack.hold_composition(held);
}

void MultiresSurface::compact_sculpt_layers() {
    if (state_) state_->stack.compact();
}

bool MultiresSurface::sculpt_layer_composition_held() const {
    return state_ && state_->stack.composition_held();
}

LocalDetail MultiresSurface::sculpt_layer_detail(SculptLayerId id, std::uint32_t level,
                                                 std::uint32_t vertex) const {
    if (!state_) return LocalDetail{};
    const DetailField* field = state_->stack.detail_at(id, level);
    return field ? field->get(vertex) : LocalDetail{};
}

// -- bake ---------------------------------------------------------------------

namespace {

// One vertex of a bake at a level above the cage: the layer's contribution
// folded into the level's own base detail. The base has neither a strength nor
// a mask, so the identity D10 has to CONSTRUCT for a merge already holds here
// and the arithmetic is a plain addition.
void bake_detail_vertex(MultiresSurface::State& s, std::uint32_t level, std::uint32_t vertex,
                        const LocalDetail& scaled, SculptLayerProperty* record) {
    MultiresLevel& lev = s.levels[level];
    const LocalDetail before = lev.detail.get(vertex);
    LocalDetail after = before;
    after.tangent += scaled.tangent;
    after.bitangent += scaled.bitangent;
    after.normal += scaled.normal;
    if (record) {
        SculptLayerProperty::DetailEntry e;
        e.level = level;
        e.vertex = vertex;
        e.before = before;
        e.after = after;
        record->base_detail.push_back(e);
    }
    lev.detail.set(vertex, after);
    lev.normals_pending.push_back(vertex);
    lev.pending.push_back(vertex);
    s.stack.invalidate(level, vertex / s.stack.block_size());
}

}  // namespace

bool MultiresSurface::bake_sculpt_layer_to_base(SculptLayerId id, SculptLayerProperty* record) {
    if (!state_) return false;
    State& s = *state_;
    if (s.stack.composition_held()) return false;
    const SculptLayer* layer = s.stack.find(id);
    if (!layer) return false;

    // Level 0 has to be evaluated before the rest frames exist, and the frames
    // are what a base deformation layer's offsets are measured in.
    evaluate_up_to(s, 0);
    const State::BaseRestFrames* rest = base_rest_frames(s);

    std::vector<std::uint8_t> before;
    if (record) before = s.stack.encode();

    const float factor = layer->composition_factor();
    const std::uint32_t bs = s.stack.block_size();
    for (std::uint32_t l = 0; l < static_cast<std::uint32_t>(s.levels.size()); ++l) {
        if (l >= layer->detail.size()) break;
        const DetailField& src = layer->detail[l];
        const std::uint32_t stored = src.stored_block_count();
        if (stored == 0) continue;
        const SparseWeightField& mask = layer->mask[l];
        for (std::uint32_t i = 0; i < stored; ++i) {
            const std::uint32_t block = src.stored_block_at(i);
            const std::uint32_t begin = block * bs;
            const std::uint32_t end = std::min(begin + bs, s.levels[l].topology.vertex_count);
            for (std::uint32_t v = begin; v < end; ++v) {
                const LocalDetail d = src.get(v);
                if (d.zero()) continue;
                const float w = factor * mask.get(v);
                LocalDetail scaled;
                scaled.tangent = w * d.tangent;
                scaled.bitangent = w * d.bitangent;
                scaled.normal = w * d.normal;
                if (l != 0) {
                    bake_detail_vertex(s, l, v, scaled, record);
                    continue;
                }
                // AT LEVEL 0 THE BASE IS THE CAGE. There is no base detail
                // there — `base_mesh()`'s meaning would change for every
                // existing caller if there were — so the offset is applied to
                // the cage position itself, in the rest frame it was authored
                // against.
                if (!rest || v >= rest->frames.size()) continue;
                const kernel::cfloat3 offset =
                    frame_to_world(rest->frames[v], scaled.tangent, scaled.bitangent,
                                   scaled.normal);
                const kernel::cfloat3 was = base_position(v);
                if (record) {
                    record->base_vertices.push_back(v);
                    record->base_before.push_back(was);
                    record->base_after.push_back(was + offset);
                }
                set_base_position(v, was + offset);
            }
        }
    }
    mark_patches(s, 0, {});
    if (!s.stack.remove(id)) return false;
    record_structural(record, id, std::move(before), s.stack);
    ++s.detail_revision;
    ++s.evaluated_revision;
    return true;
}

// -- replaying one step -------------------------------------------------------

bool MultiresSurface::apply_sculpt_layer_delta(const SculptLayerDelta& delta, bool forward) {
    if (!state_) return false;
    return forward ? delta.apply(state_->stack) : delta.revert(state_->stack);
}

bool MultiresSurface::apply_sculpt_layer_property(const SculptLayerProperty& property,
                                                  bool forward) {
    if (!state_) return false;
    State& s = *state_;
    switch (property.op) {
        case SculptLayerProperty::Op::Rename:
            return s.stack.rename(property.layer,
                                  forward ? property.name_after : property.name_before);
        case SculptLayerProperty::Op::Strength:
            return s.stack.set_strength(
                property.layer, forward ? property.strength_after : property.strength_before);
        case SculptLayerProperty::Op::Visible:
            return s.stack.set_visible(property.layer,
                                       forward ? property.flag_after : property.flag_before);
        case SculptLayerProperty::Op::Locked:
            return s.stack.set_locked(property.layer,
                                      forward ? property.flag_after : property.flag_before);
        case SculptLayerProperty::Op::Active:
            return s.stack.set_active(forward ? property.active_after : property.active_before);
        case SculptLayerProperty::Op::Structural:
            break;
    }

    // STRUCTURAL. The stack is replaced wholesale from the snapshot, and what
    // the operation wrote OUTSIDE the stack — the base detail a bake folded
    // into, and the cage a level-0 bake moved — is restored separately, because
    // neither belongs to the stack and neither could be in its bytes.
    if (s.stack.composition_held()) return false;
    const std::vector<std::uint8_t>& bytes = forward ? property.stack_after
                                                     : property.stack_before;
    SculptLayerStack rebuilt;
    if (!SculptLayerStack::decode(bytes.data(), bytes.size(), &rebuilt)) return false;

    for (const SculptLayerProperty::DetailEntry& e : property.base_detail)
        set_detail(e.level, e.vertex, forward ? e.after : e.before);
    for (std::size_t i = 0; i < property.base_vertices.size(); ++i)
        set_base_position(property.base_vertices[i],
                          forward ? property.base_after[i] : property.base_before[i]);

    s.stack = std::move(rebuilt);
    // The snapshot was taken against THIS hierarchy, but a caller can pair a
    // step with the wrong surface — so the levels are re-imposed rather than
    // trusted, which drops any field whose vertex count does not match.
    sync_stack_levels(s);
    s.stack.dirty_all();
    return true;
}

}  // namespace mesh
}  // namespace clay
