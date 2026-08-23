#include "clay/voxel/grid.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <utility>

#include "clay/mesh/transfer.h"
#include "clay/parallel/thread_pool.h"
#include "dither.h"

namespace clay {
namespace voxel {

using kernel::cf3;
using kernel::cfloat3;

namespace {
// floor division/modulo for negative lattice coordinates
inline std::int32_t fdiv(std::int32_t a, std::int32_t b) {
    return (a >= 0) ? a / b : -(((-a) + b - 1) / b);
}
inline std::int32_t fmod_pos(std::int32_t a, std::int32_t b) { return a - fdiv(a, b) * b; }

// The cell one level coarser that contains this one, and the first of its
// eight children. Floor division, so the mapping stays a partition across the
// origin instead of folding negative cells onto positive ones.
inline VoxelCoord parent_cell(VoxelCoord c) { return {fdiv(c.x, 2), fdiv(c.y, 2), fdiv(c.z, 2)}; }
inline VoxelCoord child_cell(VoxelCoord parent, int i) {
    return {parent.x * 2 + (i & 1), parent.y * 2 + ((i >> 1) & 1), parent.z * 2 + ((i >> 2) & 1)};
}
inline constexpr int kChildren = 8;

// A cell whose children would leave the int32 lattice has none. Doubling is
// what a level costs, and a subdivision that wrapped would put material at a
// coordinate nobody asked for — silently, and for a cell nothing can reach.
inline bool has_children(VoxelCoord c) {
    constexpr std::int32_t kLimit = std::numeric_limits<std::int32_t>::max() / 2;
    return c.x <= kLimit && c.x >= -kLimit && c.y <= kLimit && c.y >= -kLimit && c.z <= kLimit &&
           c.z >= -kLimit;
}
}  // namespace

VoxelCoord VoxelGrid::chunk_key(VoxelCoord c) {
    return {fdiv(c.x, kChunkDim), fdiv(c.y, kChunkDim), fdiv(c.z, kChunkDim)};
}

std::size_t VoxelGrid::chunk_offset(VoxelCoord c) {
    std::int32_t x = fmod_pos(c.x, kChunkDim);
    std::int32_t y = fmod_pos(c.y, kChunkDim);
    std::int32_t z = fmod_pos(c.z, kChunkDim);
    return (static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x;
}

// -- palette -----------------------------------------------------------------

std::uint8_t VoxelGrid::palette_add(cfloat3 color, float tolerance) {
    for (std::size_t i = 1; i < palette_.size(); ++i)
        if (kernel::clength(palette_[i] - color) <= tolerance)
            return static_cast<std::uint8_t>(i);
    if (palette_.size() >= 256) return static_cast<std::uint8_t>(palette_.size() - 1);
    palette_.push_back(color);
    return static_cast<std::uint8_t>(palette_.size() - 1);
}

cfloat3 VoxelGrid::palette_color(std::uint8_t index) const {
    return index < palette_.size() ? palette_[index] : cf3(0, 0, 0);
}

void VoxelGrid::palette_set(std::uint8_t index, cfloat3 color) {
    if (index > 0 && index < palette_.size()) palette_[index] = color;
}

// -- single voxel ------------------------------------------------------------

// For a chunk a level does not store, the single ancestor chunk whose cells it
// mirrors, and how many levels up that is. One coarse chunk covers exactly the
// eight fine chunks below it, so walking up is a halving per level rather than
// a search.
//
// This exists so that reading an inherited chunk costs ONE map lookup rather
// than a recursive cell_at per cell. bounds_min/bounds_max are called per ray
// by the raycaster; a hash and a recursion per cell there is the difference
// between a render and a hang.
const VoxelGrid::Chunk* VoxelGrid::inherited_chunk(std::size_t level, VoxelCoord key,
                                                   int* out_up) const {
    int up = 0;
    while (level > 0 && !chunk_is_refined(level, key)) {
        key = {fdiv(key.x, 2), fdiv(key.y, 2), fdiv(key.z, 2)};
        --level;
        ++up;
    }
    *out_up = up;
    auto it = levels_[level].chunks.find(key);
    return it == levels_[level].chunks.end() ? nullptr : &it->second;
}

bool VoxelGrid::chunk_is_refined(std::size_t level, VoxelCoord key) const {
    const Level& lv = levels_[level];
    return lv.whole || lv.refined.find(key) != lv.refined.end();
}

std::uint8_t VoxelGrid::cell_at(std::size_t level, VoxelCoord c) const {
    const VoxelCoord key = chunk_key(c);
    // An unrefined chunk is not empty — it is UNSTORED, and its value is the
    // one its parent carries. Reading it as empty is the single mistake that
    // would turn regional refinement from a storage change into a hole in the
    // solid, so the fallback comes before the chunk lookup rather than after a
    // miss.
    if (level > 0 && !chunk_is_refined(level, key)) return cell_at(level - 1, parent_cell(c));
    const ChunkMap& chunks = levels_[level].chunks;
    auto it = chunks.find(key);
    return it == chunks.end() ? 0 : it->second.data[chunk_offset(c)];
}

// Returns whether the cell actually changed, which is what change_count counts.
bool VoxelGrid::write_cell(std::size_t level, VoxelCoord c, std::uint8_t index) {
    VoxelCoord key = chunk_key(c);
    // Writing where the level has no storage gives it some. Refusing instead
    // would break every brush whose footprint straddles a boundary, which is
    // the common case rather than the exceptional one.
    if (level > 0 && !chunk_is_refined(level, key)) refine_chunk(level, key);
    ChunkMap& chunks = levels_[level].chunks;
    auto it = chunks.find(key);
    if (it == chunks.end()) {
        if (index == 0) return false;
        it = chunks.emplace(key, Chunk{}).first;
        it->second.data.assign(static_cast<std::size_t>(kChunkDim) * kChunkDim * kChunkDim, 0);
    }
    std::uint8_t& cell = it->second.data[chunk_offset(c)];
    bool changed = cell != index;
    if (cell == 0 && index != 0) ++it->second.occupied;
    if (cell != 0 && index == 0) --it->second.occupied;
    cell = index;
    // A chunk that reaches zero occupancy goes, because a missing chunk is how
    // this grid spells empty space. It is marked dirty BELOW rather than
    // skipped: a host holding its quads has to be told to drop them.
    if (it->second.occupied == 0) chunks.erase(it);
    if (changed) {
        mark_chunk_dirty(level, c, key);
        // Any level ABOVE this one inherits from it, so a write here can move
        // their extents too. Cheap to drop them all; the walk only re-runs
        // when something asks.
        for (std::size_t i = level; i < levels_.size(); ++i) levels_[i].bounds_valid = false;
    }
    return changed;
}

// A write dirties its own chunk, and the chunk across any face it touches.
//
// The mask build reads the neighbour cell to decide whether a face is exposed,
// and on a chunk's boundary slice that read crosses into the next chunk. So a
// write here changes what THAT chunk emits, and a host re-meshing only the
// written chunk would leave the neighbour's stale quads on screen — a hole (or
// a doubled wall) that shows up only at chunk boundaries.
//
// A neighbour that does not exist is left alone. An absent chunk holds no
// material and emits no quads, and any later write into it marks it itself, so
// the set stays proportional to the material rather than to the surface area of
// everywhere ever touched.
void VoxelGrid::mark_chunk_dirty(std::size_t level, VoxelCoord c, VoxelCoord key) {
    Level& lv = levels_[level];
    const std::int32_t lx = fmod_pos(c.x, kChunkDim);
    const std::int32_t ly = fmod_pos(c.y, kChunkDim);
    const std::int32_t lz = fmod_pos(c.z, kChunkDim);
    const bool on_face = lx == 0 || lx == kChunkDim - 1 || ly == 0 || ly == kChunkDim - 1 ||
                         lz == 0 || lz == kChunkDim - 1;
    // The memo: this is the write path, charged per cell a rasterize touches,
    // and an interior cell of a chunk already marked has nothing left to do.
    if (!on_face && lv.dirty_memo_valid && key == lv.dirty_memo) return;
    lv.dirty.insert(key);
    lv.dirty_memo = key;
    lv.dirty_memo_valid = true;
    if (!on_face) return;
    auto neighbour = [&](std::int32_t dx, std::int32_t dy, std::int32_t dz) {
        VoxelCoord n{key.x + dx, key.y + dy, key.z + dz};
        if (lv.chunks.find(n) != lv.chunks.end()) {
            lv.dirty.insert(n);
            return;
        }
        // A chunk this level does not STORE can still carry material, inherited
        // from its parent — and it emits faces across this boundary just as a
        // stored one would. Asking the cell rather than the chunk map is what
        // keeps a host from leaving stale quads on a neighbour it was never
        // told about.
        if (level == 0 || lv.whole) return;
        const VoxelCoord across{c.x + dx, c.y + dy, c.z + dz};
        if (cell_at(level, across) != 0) lv.dirty.insert(n);
    };
    if (lx == 0) neighbour(-1, 0, 0);
    if (lx == kChunkDim - 1) neighbour(1, 0, 0);
    if (ly == 0) neighbour(0, -1, 0);
    if (ly == kChunkDim - 1) neighbour(0, 1, 0);
    if (lz == 0) neighbour(0, 0, -1);
    if (lz == kChunkDim - 1) neighbour(0, 0, 1);
}

std::vector<VoxelCoord> VoxelGrid::take_dirty_chunks(std::size_t level) {
    std::vector<VoxelCoord> keys;
    if (level >= levels_.size()) return keys;
    Level& lv = levels_[level];
    keys.assign(lv.dirty.begin(), lv.dirty.end());
    lv.dirty.clear();
    lv.dirty_memo_valid = false;  // the set is empty; the next write must insert
    // Sorted rather than in the set's own order, so two runs of the same edit
    // sequence hand a host the same keys in the same order and mesh them in
    // the same order.
    std::sort(keys.begin(), keys.end(), [](VoxelCoord a, VoxelCoord b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    return keys;
}

std::size_t VoxelGrid::dirty_chunk_count(std::size_t level) const {
    return level < levels_.size() ? levels_[level].dirty.size() : 0;
}

std::uint8_t VoxelGrid::get(VoxelCoord c) const { return cell_at(active_, c); }

void VoxelGrid::read_region(VoxelCoord lo, VoxelCoord hi, std::uint8_t* out) const {
    if (!out) return;
    const ChunkMap& chunks = levels_[active_].chunks;
    std::size_t i = 0;
    for (std::int32_t z = lo.z; z <= hi.z; ++z)
        for (std::int32_t y = lo.y; y <= hi.y; ++y) {
            std::int32_t x = lo.x;
            while (x <= hi.x) {
                const VoxelCoord key = chunk_key({x, y, z});
                // This chunk covers x up to its own last column; the run ends
                // there or at the caller's box, whichever comes first.
                const std::int32_t chunk_last = (key.x + 1) * kChunkDim - 1;
                const std::int32_t run_last = chunk_last < hi.x ? chunk_last : hi.x;

                // ONE resolve for the whole run — the point of this function.
                int up = 0;
                auto it = chunks.find(key);
                const Chunk* src =
                    it != chunks.end() ? &it->second : inherited_chunk(active_, key, &up);
                if (!src) {
                    for (std::int32_t cx = x; cx <= run_last; ++cx) out[i++] = 0;
                    x = run_last + 1;
                    continue;
                }
                for (std::int32_t cx = x; cx <= run_last; ++cx) {
                    // An inherited chunk mirrors its ancestor's cells, so the
                    // read is that chunk's data at the shifted coordinate.
                    const std::int32_t ax = up == 0 ? cx : (cx >> up);
                    const std::int32_t ay = up == 0 ? y : (y >> up);
                    const std::int32_t az = up == 0 ? z : (z >> up);
                    const std::size_t ox = static_cast<std::size_t>(fmod_pos(ax, kChunkDim));
                    const std::size_t oy = static_cast<std::size_t>(fmod_pos(ay, kChunkDim));
                    const std::size_t oz = static_cast<std::size_t>(fmod_pos(az, kChunkDim));
                    out[i++] = src->data[(oz * kChunkDim + oy) * kChunkDim + ox];
                }
                x = run_last + 1;
            }
        }
}

std::uint8_t VoxelGrid::cell_index(std::size_t level, VoxelCoord c) const {
    return level < levels_.size() ? cell_at(level, c) : 0;
}

void VoxelGrid::set(VoxelCoord c, std::uint8_t index) {
    // Every verb funnels its writes through here, so one compare instruments
    // all of them, and propagation below is charged to the edit rather than
    // counted as further edits — and one recording hook attributes all of them
    // to the open sculpt layer.
    if (recording_) {
        SculptLayerRecord& rec = sculpt_layers_.back();
        auto it = rec.index.find(c);
        if (it == rec.index.end()) {
            // FIRST touch keeps the before: the pass's starting state for this
            // cell, whatever it does to it afterwards.
            rec.index.emplace(c, rec.changes.size());
            rec.changes.push_back({c, get(c), index});
        } else {
            rec.changes[it->second].after = index;
        }
    }
    // The undo journal, if one is installed. Two differences from the sculpt
    // layer hook above, both deliberate.
    //
    // NOT COALESCED BY CELL. A sculpt layer is a pass whose NET effect per cell
    // is what a strength dial re-picks; a replay must unwind the writes in the
    // order they were made, and the verbs that propagate through levels write a
    // cell more than once.
    //
    // ONLY WRITES THAT CHANGED SOMETHING. write_cell says whether it did, and
    // journaling the ones that did not would build undo steps that undo
    // nothing — erasing an already-empty cell, a flatten meeting a flat
    // region, a dab landing on empty space. All of those are ordinary here,
    // and an undo that does nothing is the exact defect this channel exists to
    // avoid. The sculpt-layer hook above keeps recording them, because a pass's
    // membership is not the same question.
    const std::uint8_t before = change_sink_ ? get(c) : std::uint8_t{0};
    if (write_cell(active_, c, index)) {
        ++change_count_;
        if (change_sink_) change_sink_->push_back({c, before, index});
    }
    if (levels_.size() == 1) return;  // the single-level grid: nothing to carry
    // Down first: it restates this cell's own offset against the parent it just
    // recomputed, which is what the walk back up then replays.
    propagate_down(active_, c);
    propagate_up(active_, c);
}

// -- sculpt layers -----------------------------------------------------------

std::size_t VoxelGrid::begin_sculpt_layer(std::string name) {
    end_sculpt_layer();
    SculptLayerRecord rec;
    rec.name = std::move(name);
    rec.seed = next_sculpt_seed_++;
    sculpt_layers_.push_back(std::move(rec));
    recording_ = true;
    return sculpt_layers_.size() - 1;
}

void VoxelGrid::end_sculpt_layer() { recording_ = false; }

const std::string& VoxelGrid::sculpt_layer_name(std::size_t layer) const {
    static const std::string kNone;
    return layer < sculpt_layers_.size() ? sculpt_layers_[layer].name : kNone;
}

std::size_t VoxelGrid::sculpt_layer_cell_count(std::size_t layer) const {
    return layer < sculpt_layers_.size() ? sculpt_layers_[layer].changes.size() : 0;
}

float VoxelGrid::sculpt_layer_strength(std::size_t layer) const {
    return layer < sculpt_layers_.size() ? sculpt_layers_[layer].strength : 0.0f;
}

bool VoxelGrid::sculpt_layer_visible(std::size_t layer) const {
    return layer < sculpt_layers_.size() && sculpt_layers_[layer].visible;
}

// Undo every layer from `first` upward, TOP DOWN.
//
// The order is the whole correctness argument. A layer's `before` was captured
// against the state at the moment that layer ran — which is the base plus every
// layer below it — so restoring them in reverse order walks that history
// backwards exactly. Reverting bottom-up would restore a value that a later
// layer had already overwritten.
// Backwards, restoring each write's `before`. Not a pass: the recording hooks
// are both suspended, exactly as a sculpt-layer recompose suspends its own.
void VoxelGrid::revert_changes(const std::vector<SculptChange>& changes) {
    const bool was_recording = recording_;
    std::vector<SculptChange>* was_sink = change_sink_;
    recording_ = false;
    change_sink_ = nullptr;
    for (std::size_t i = changes.size(); i > 0; --i)
        set(changes[i - 1].cell, changes[i - 1].before);
    recording_ = was_recording;
    change_sink_ = was_sink;
}

// Forwards, restoring each write's `after`.
void VoxelGrid::reapply_changes(const std::vector<SculptChange>& changes) {
    const bool was_recording = recording_;
    std::vector<SculptChange>* was_sink = change_sink_;
    recording_ = false;
    change_sink_ = nullptr;
    for (const SculptChange& ch : changes) set(ch.cell, ch.after);
    recording_ = was_recording;
    change_sink_ = was_sink;
}

void VoxelGrid::revert_from(std::size_t first) {
    const bool was_recording = recording_;
    recording_ = false;  // a recompose is not a pass
    for (std::size_t i = sculpt_layers_.size(); i > first; --i) {
        const SculptLayerRecord& rec = sculpt_layers_[i - 1];
        if (!rec.visible) continue;
        // Backwards within the layer too: a pass that wrote one cell twice
        // recorded only its first `before`, but a replay of the OTHER cells
        // must still unwind in the order it laid them down.
        for (std::size_t c = rec.changes.size(); c > 0; --c) {
            const SculptChange& ch = rec.changes[c - 1];
            if (dither::passes(ch.cell, rec.strength, rec.seed)) set(ch.cell, ch.before);
        }
    }
    recording_ = was_recording;
}

// Replay every layer from `first` upward, bottom up, at its current strength.
void VoxelGrid::apply_from(std::size_t first) {
    const bool was_recording = recording_;
    recording_ = false;
    for (std::size_t i = first; i < sculpt_layers_.size(); ++i) {
        const SculptLayerRecord& rec = sculpt_layers_[i];
        if (!rec.visible) continue;
        for (const SculptChange& ch : rec.changes)
            if (dither::passes(ch.cell, rec.strength, rec.seed)) set(ch.cell, ch.after);
    }
    recording_ = was_recording;
}

bool VoxelGrid::set_sculpt_layer_strength(std::size_t layer, float strength) {
    if (layer >= sculpt_layers_.size()) return false;
    const float s = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    if (s == sculpt_layers_[layer].strength) return true;
    revert_from(layer);
    sculpt_layers_[layer].strength = s;
    apply_from(layer);
    return true;
}

bool VoxelGrid::set_sculpt_layer_visible(std::size_t layer, bool visible) {
    if (layer >= sculpt_layers_.size()) return false;
    if (visible == sculpt_layers_[layer].visible) return true;
    revert_from(layer);
    sculpt_layers_[layer].visible = visible;
    apply_from(layer);
    return true;
}

bool VoxelGrid::remove_sculpt_layer(std::size_t layer) {
    if (layer >= sculpt_layers_.size()) return false;
    revert_from(layer);
    sculpt_layers_.erase(sculpt_layers_.begin() + static_cast<std::ptrdiff_t>(layer));
    apply_from(layer);
    return true;
}

bool VoxelGrid::merge_sculpt_layer_down(std::size_t layer) {
    // Nothing below to merge into.
    if (layer == 0 || layer >= sculpt_layers_.size()) return false;
    // Composed at FULL strength, because a merged pass is one pass: keeping
    // the upper layer's dither would bake a fractional subset in and leave the
    // result unable to reach the other cells again.
    revert_from(layer - 1);
    SculptLayerRecord upper = std::move(sculpt_layers_[layer]);
    sculpt_layers_.erase(sculpt_layers_.begin() + static_cast<std::ptrdiff_t>(layer));
    SculptLayerRecord& lower = sculpt_layers_[layer - 1];
    for (const SculptChange& ch : upper.changes) {
        auto it = lower.index.find(ch.cell);
        if (it == lower.index.end()) {
            lower.index.emplace(ch.cell, lower.changes.size());
            lower.changes.push_back(ch);
        } else {
            // The lower layer already owns this cell: keep ITS before — that
            // is the state both passes started from — and take the upper's
            // after, which is where the pair ends up.
            lower.changes[it->second].after = ch.after;
        }
    }
    apply_from(layer - 1);
    return true;
}

bool VoxelGrid::move_sculpt_layer(std::size_t from, std::size_t to) {
    if (from >= sculpt_layers_.size() || to >= sculpt_layers_.size()) return false;
    if (from == to) return true;
    // Everything from the lower of the two positions is affected, so that is
    // where the unwind starts.
    const std::size_t first = std::min(from, to);
    revert_from(first);
    SculptLayerRecord moved = std::move(sculpt_layers_[from]);
    sculpt_layers_.erase(sculpt_layers_.begin() + static_cast<std::ptrdiff_t>(from));
    sculpt_layers_.insert(sculpt_layers_.begin() + static_cast<std::ptrdiff_t>(to),
                          std::move(moved));
    apply_from(first);
    return true;
}

std::size_t VoxelGrid::sculpt_layer_bytes(std::size_t layer) const {
    if (layer >= sculpt_layers_.size()) return 0;
    const SculptLayerRecord& rec = sculpt_layers_[layer];
    // The changes, plus the lookup that keeps recording O(1) per write. The
    // node overhead of an unordered_map is not observable from here, so this
    // counts key and value and says so rather than guessing at an allocator.
    return rec.changes.size() * sizeof(SculptChange) +
           rec.index.size() * (sizeof(VoxelCoord) + sizeof(std::size_t)) + rec.name.capacity();
}

std::size_t VoxelGrid::sculpt_layer_total_bytes() const {
    std::size_t total = 0;
    for (std::size_t i = 0; i < sculpt_layers_.size(); ++i) total += sculpt_layer_bytes(i);
    return total;
}

void VoxelGrid::paint(VoxelCoord c, std::uint8_t index) {
    if (index != 0 && get(c) != 0) set(c, index);
}

// -- resolution levels -------------------------------------------------------

float VoxelGrid::level_voxel_size(std::size_t level) const {
    return level < levels_.size() ? levels_[level].voxel_size : 0.0f;
}

std::size_t VoxelGrid::level_occupied_count(std::size_t level) const {
    if (level >= levels_.size()) return 0;
    std::size_t n = 0;
    for (const auto& [key, chunk] : levels_[level].chunks)
        n += static_cast<std::size_t>(chunk.occupied);
    if (level == 0 || levels_[level].whole) return n;
    // A partially refined level HAS the material its unrefined chunks inherit,
    // and this counts the solid rather than the storage — the same solid
    // bounds() describes and the mesher meshes. Reporting storage here would
    // leave two accessors disagreeing about what a level is; what a level COSTS
    // is level_refined_chunk_count, which is what memory actually follows.
    //
    // Walked per cell only for the inherited part, and only on a partial level.
    for (const VoxelCoord& key : material_chunk_keys(level)) {
        if (chunk_is_refined(level, key)) continue;  // counted above
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x)
                    if (cell_at(level, {key.x * kChunkDim + x, key.y * kChunkDim + y,
                                        key.z * kChunkDim + z}) != 0)
                        ++n;
    }
    return n;
}

bool VoxelGrid::set_active_level(std::size_t level) {
    if (level >= levels_.size()) return false;
    active_ = level;
    return true;
}

// Capped because the file format has to be: a stream naming an arbitrary level
// count would otherwise be a request to allocate one. Memory refuses a stack
// this deep long before the cap does — every level costs eight times the cells
// of the one below.
std::size_t VoxelGrid::add_level() {
    if (levels_.size() >= kMaxLevels) return levels_.size() - 1;
    const float half = levels_.back().voxel_size * 0.5f;
    levels_.emplace_back().voxel_size = half;
    std::size_t fine = levels_.size() - 1;
    subdivide_into(fine);
    return fine;
}

std::size_t VoxelGrid::add_level(const math::Aabb& region) {
    if (levels_.size() >= kMaxLevels) return levels_.size() - 1;
    const float half = levels_.back().voxel_size * 0.5f;
    Level& lv = levels_.emplace_back();
    lv.voxel_size = half;
    lv.whole = false;
    std::size_t fine = levels_.size() - 1;
    if (region.empty()) return fine;  // a level that stores nothing reads as its parent

    // World bounds to fine cells to chunk keys, rounded OUT at both ends: a
    // region is a request for detail somewhere, and half a chunk of detail is
    // not a thing this grid can store.
    auto key_of = [&](float v) {
        return static_cast<std::int32_t>(
            std::floor(std::floor(v / half) / static_cast<float>(kChunkDim)));
    };
    const std::int32_t x0 = key_of(region.min.x), x1 = key_of(region.max.x);
    const std::int32_t y0 = key_of(region.min.y), y1 = key_of(region.max.y);
    const std::int32_t z0 = key_of(region.min.z), z1 = key_of(region.max.z);
    for (std::int32_t z = z0; z <= z1; ++z)
        for (std::int32_t y = y0; y <= y1; ++y)
            for (std::int32_t x = x0; x <= x1; ++x) levels_[fine].refined.insert({x, y, z});

    seed_refined_chunks(fine);
    return fine;
}

// Chunk keys where this level HAS material, which is not the same as the
// chunks it STORES: an unrefined chunk reads its parent, so the parent's
// material is at this level too.
//
// Every consumer that enumerates chunks — bounds, the meshing slabs, the dirty
// set — asks this rather than walking `chunks`. Reading VALUES was already
// safe, because the sweeps go through `cell_at`; what a region breaks is the
// question "which chunks are worth visiting", and that is the whole of it.
std::vector<VoxelCoord> VoxelGrid::material_chunk_keys(std::size_t level) const {
    std::vector<VoxelCoord> keys;
    if (level >= levels_.size()) return keys;
    const Level& lv = levels_[level];
    keys.reserve(lv.chunks.size());
    for (const auto& [key, chunk] : lv.chunks)
        if (chunk.occupied > 0) keys.push_back(key);
    if (level == 0 || lv.whole) return keys;
    // One coarse chunk's cells subdivide into exactly eight fine chunks, so the
    // inherited keys are that expansion — minus the ones this level stores,
    // which are already above.
    for (const VoxelCoord& pk : material_chunk_keys(level - 1))
        for (int i = 0; i < 8; ++i) {
            const VoxelCoord k{pk.x * 2 + (i & 1), pk.y * 2 + ((i >> 1) & 1),
                               pk.z * 2 + ((i >> 2) & 1)};
            if (!chunk_is_refined(level, k)) keys.push_back(k);
        }
    return keys;
}

std::size_t VoxelGrid::level_refined_chunk_count(std::size_t level) const {
    if (level >= levels_.size()) return 0;
    return levels_[level].whole ? levels_[level].chunks.size() : levels_[level].refined.size();
}

bool VoxelGrid::level_is_whole(std::size_t level) const {
    return level < levels_.size() && levels_[level].whole;
}

// Fill one already-refined chunk with what its parent says, so giving a level
// storage cannot change the solid.
//
// Read through cell_at on the PARENT rather than off the parent's chunk data:
// the level below may itself be partial, and the material in the chunks it
// does not store is exactly as real as the material in the ones it does. A
// seeding that walked only stored chunks leaves a hole one chunk wide
// wherever a region is stacked over an inherited area.
void VoxelGrid::seed_chunk(std::size_t level, VoxelCoord key) {
    for (int z = 0; z < kChunkDim; ++z)
        for (int y = 0; y < kChunkDim; ++y)
            for (int x = 0; x < kChunkDim; ++x) {
                const VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                   key.z * kChunkDim + z};
                const std::uint8_t v = cell_at(level - 1, parent_cell(c));
                if (v != 0) write_cell(level, c, v);
            }
}

// Storage for one chunk, seeded from the parent so the solid does not move.
void VoxelGrid::refine_chunk(std::size_t level, VoxelCoord key) {
    if (level == 0 || chunk_is_refined(level, key)) return;
    // Upward-closed: a fine edit propagates DOWN, and needs somewhere coarse to
    // land. Two fine chunks per axis share one coarse chunk, so the ancestor is
    // a single key rather than a range.
    refine_chunk(level - 1, {fdiv(key.x, 2), fdiv(key.y, 2), fdiv(key.z, 2)});
    // Marked BEFORE seeding: the seeding writes go through write_cell, which
    // would otherwise see an unrefined chunk and recurse into here forever.
    levels_[level].refined.insert(key);
    seed_chunk(level, key);
}

// Every chunk a partially refined level was given, filled from the level below.
void VoxelGrid::seed_refined_chunks(std::size_t fine) {
    // Copied, because seeding writes propagate and may refine further chunks.
    const std::vector<VoxelCoord> keys(levels_[fine].refined.begin(),
                                       levels_[fine].refined.end());
    for (const VoxelCoord& key : keys) seed_chunk(fine, key);
}

bool VoxelGrid::drop_level() {
    if (levels_.size() < 2) return false;
    levels_.pop_back();
    if (active_ >= levels_.size()) active_ = levels_.size() - 1;
    return true;
}

// Seed a freshly added level from the one below it: every occupied coarse cell
// becomes its eight children with the same palette index. The solid is exactly
// the same one, so adding a level cannot move the surface, and no cell differs
// from its parent — the detail map starts empty.
void VoxelGrid::subdivide_into(std::size_t fine) {
    const ChunkMap& coarse = levels_[fine - 1].chunks;
    // A WHOLE level refines everywhere, so the per-child refinement test below
    // is constant-true — and reaching it costs a chunk_key(), which is three
    // divisions, for each of the eight children of every material cell. Hoisted
    // rather than trusted to the short-circuit inside chunk_is_refined: the
    // short-circuit saves the set lookup, not the key. Measured on the device
    // gate at 2.36x (voxel_add_level, 0.51 -> 1.21 ms) when the region-refined
    // path added the test.
    const bool whole = levels_[fine].whole;
    for (const auto& [key, chunk] : coarse) {
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    std::uint8_t v =
                        chunk.data[(static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x];
                    if (v == 0) continue;
                    VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                 key.z * kChunkDim + z};
                    if (!has_children(c)) continue;
                    for (int i = 0; i < kChildren; ++i) {
                        const VoxelCoord child = child_cell(c, i);
                        // Outside the refined set the fine level already READS
                        // as this cell, so writing it would only buy storage.
                        if (!whole && !chunk_is_refined(fine, chunk_key(child))) continue;
                        write_cell(fine, child, v);
                    }
                }
    }
}

// A cell differs from its parent or it does not; the map holds exactly the ones
// that do, so it stays the size of the detail rather than the size of the level.
void VoxelGrid::record_detail(std::size_t level, VoxelCoord c) {
    if (level == 0) return;
    // An unrefined cell reads as its parent BY DEFINITION, so it can never be
    // an offset against it. Recording one would put an entry in the map for a
    // cell the level does not store, and the map is meant to be exactly the
    // cells that differ.
    //
    // The `whole` test is spelled out rather than left to the short-circuit
    // inside chunk_is_refined, for the reason #137 gives about subdivide_into:
    // the short-circuit saves the set lookup and not the chunk_key(), which is
    // three divisions. This runs once per child of every propagated write.
    if (!levels_[level].whole && !chunk_is_refined(level, chunk_key(c))) return;
    std::uint8_t v = cell_at(level, c);
    if (v == cell_at(level - 1, parent_cell(c)))
        levels_[level].detail.erase(c);
    else
        levels_[level].detail[c] = v;
}

void VoxelGrid::refresh_detail(std::size_t level, VoxelCoord parent) {
    for (int i = 0; i < kChildren; ++i) record_detail(level, child_cell(parent, i));
}

// Average, not subsample: a coarse cell is occupied when at least half its
// eight children are, coloured by the commonest child. Picking one child would
// make dropping a level depend on which corner the sculptor happened to hit.
std::uint8_t VoxelGrid::downsample_cell(std::size_t fine, VoxelCoord coarse) const {
    int tally[256] = {0};
    int occupied = 0;
    for (int i = 0; i < kChildren; ++i) {
        std::uint8_t v = cell_at(fine, child_cell(coarse, i));
        if (v == 0) continue;
        ++occupied;
        ++tally[v];
    }
    if (occupied * 2 < kChildren) return 0;
    std::uint8_t best = 0;
    int best_count = 0;
    for (int i = 1; i < 256; ++i)
        if (tally[i] > best_count) {
            best_count = tally[i];
            best = static_cast<std::uint8_t>(i);
        }
    return best;
}

void VoxelGrid::propagate_down(std::size_t from, VoxelCoord c) {
    for (std::size_t fine = from; fine > 0; --fine) {
        VoxelCoord parent = parent_cell(c);
        write_cell(fine - 1, parent, downsample_cell(fine, parent));
        // The parent moved, so every child's offset against it is restated —
        // including the siblings this edit never touched.
        refresh_detail(fine, parent);
        c = parent;
    }
}

void VoxelGrid::propagate_up(std::size_t from, VoxelCoord c) {
    if (from + 1 >= levels_.size() || !has_children(c)) return;
    const std::size_t fine = from + 1;
    const std::uint8_t predicted = cell_at(from, c);
    // Hoisted for the same reason record_detail above spells it out: constant
    // on a whole level, and reaching it costs a chunk_key() per child. This
    // recurses, so a two-level stack pays it eight times per write and a
    // three-level stack sixty-four.
    const bool whole = levels_[fine].whole;
    for (int i = 0; i < kChildren; ++i) {
        VoxelCoord child = child_cell(c, i);
        // An unrefined chunk already reads as its parent, so there is nothing
        // to replay into it — and writing would materialise exactly the storage
        // the region exists to avoid.
        if (!whole && !chunk_is_refined(fine, chunk_key(child))) continue;
        auto& detail = levels_[fine].detail;
        auto it = detail.find(child);
        std::uint8_t v = it != detail.end() ? it->second : predicted;
        write_cell(fine, child, v);
        // An offset the coarse form has caught up with is no offset at all.
        // Dropping it keeps the map exactly the cells that differ, which is
        // what makes the serialised stream a canonical form of the grid.
        if (it != detail.end() && v == predicted) detail.erase(it);
        propagate_up(fine, child);
    }
}

// -- brushes and fills -------------------------------------------------------

namespace {

// Footprint of a size-n brush along one axis: n cells for every n, symmetric
// for odd n and biased half a cell toward + for even n.
struct BrushExtent {
    int lo, hi;
};
inline BrushExtent brush_extent(int n) { return {-((n - 1) / 2), n / 2}; }

// Whether cell offset (x, y, z) lies inside the brush. The sphere admits
// cells whose centre is within radius n/2 of the footprint centre. The
// centre is half-integer for even n, so the test works in half-units
// (doubling everything) and stays exact integer arithmetic:
//     (2x - (lo+hi))^2 + ... <= n^2
inline bool in_brush(int x, int y, int z, int n, BrushExtent e, BrushShape shape) {
    if (shape == BrushShape::Cube) return true;
    int mid = e.lo + e.hi;  // == 2 * centre
    int dx = 2 * x - mid, dy = 2 * y - mid, dz = 2 * z - mid;
    return dx * dx + dy * dy + dz * dz <= n * n;
}

}  // namespace

void VoxelGrid::set_brush(VoxelCoord c, int n, std::uint8_t index, BrushShape shape) {
    BrushExtent e = brush_extent(n);
    for (int z = e.lo; z <= e.hi; ++z)
        for (int y = e.lo; y <= e.hi; ++y)
            for (int x = e.lo; x <= e.hi; ++x)
                if (in_brush(x, y, z, n, e, shape)) set({c.x + x, c.y + y, c.z + z}, index);
}

void VoxelGrid::paint_brush(VoxelCoord c, int n, std::uint8_t index, BrushShape shape) {
    BrushExtent e = brush_extent(n);
    for (int z = e.lo; z <= e.hi; ++z)
        for (int y = e.lo; y <= e.hi; ++y)
            for (int x = e.lo; x <= e.hi; ++x)
                if (in_brush(x, y, z, n, e, shape)) paint({c.x + x, c.y + y, c.z + z}, index);
}

void VoxelGrid::fill_box(VoxelCoord a, VoxelCoord b, std::uint8_t index) {
    VoxelCoord lo{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    VoxelCoord hi{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    for (std::int32_t z = lo.z; z <= hi.z; ++z)
        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) set({x, y, z}, index);
}

void VoxelGrid::fill_line(VoxelCoord a, VoxelCoord b, std::uint8_t index) {
    // 3D DDA over cell centers
    int steps = std::max({std::abs(b.x - a.x), std::abs(b.y - a.y), std::abs(b.z - a.z)});
    if (steps == 0) {
        set(a, index);
        return;
    }
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        VoxelCoord c{static_cast<std::int32_t>(std::lround(a.x + (b.x - a.x) * t)),
                     static_cast<std::int32_t>(std::lround(a.y + (b.y - a.y) * t)),
                     static_cast<std::int32_t>(std::lround(a.z + (b.z - a.z) * t))};
        set(c, index);
    }
}

// -- mirror ------------------------------------------------------------------

VoxelCoord VoxelGrid::mirrored(VoxelCoord c, std::uint8_t axes) {
    VoxelCoord m = c;
    if (axes & kVoxMirrorX) m.x = -1 - m.x;
    if (axes & kVoxMirrorY) m.y = -1 - m.y;
    if (axes & kVoxMirrorZ) m.z = -1 - m.z;
    return m;
}

namespace {
template <typename Fn>
void for_each_mirror(VoxelCoord c, std::uint8_t axes, Fn fn) {
    for (std::uint8_t combo = 0; combo < 8; ++combo) {
        if ((combo & ~axes) != 0) continue;  // only active axes participate
        fn(VoxelGrid::mirrored(c, combo));
    }
}
}  // namespace

void VoxelGrid::set_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes) {
    for_each_mirror(c, axes, [&](VoxelCoord m) { set(m, index); });
}

void VoxelGrid::paint_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes) {
    for_each_mirror(c, axes, [&](VoxelCoord m) { paint(m, index); });
}

// -- queries -----------------------------------------------------------------

std::size_t VoxelGrid::occupied_count() const { return level_occupied_count(active_); }

// One walk for both ends, cached on the level.
//
// bounds_min/bounds_max used to walk every material cell each, and
// raycast_voxels asks for both PER RAY — so rendering a grid paid two full
// walks per pixel, which was the whole of a 29 s render on a 65k-cell sculpt.
// Computing them together and remembering the answer makes the walk happen
// once per edit instead of once per ray.
void VoxelGrid::ensure_bounds() const {
    Level& lv = const_cast<Level&>(levels_[active_]);
    if (lv.bounds_valid) return;
    bool any = false;
    VoxelCoord lo{}, hi{};
    const ChunkMap& chunks = lv.chunks;
    for (const VoxelCoord& key : material_chunk_keys(active_)) {
        auto it = chunks.find(key);
        int up = 0;
        const Chunk* src = it != chunks.end() ? &it->second : inherited_chunk(active_, key, &up);
        if (!src) continue;
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    const VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                       key.z * kChunkDim + z};
                    // An inherited chunk mirrors its ancestor's cells, so the
                    // read is that chunk's data at the shifted coordinate.
                    const VoxelCoord a =
                        up == 0 ? c : VoxelCoord{c.x >> up, c.y >> up, c.z >> up};
                    const std::size_t ox = static_cast<std::size_t>(fmod_pos(a.x, kChunkDim));
                    const std::size_t oy = static_cast<std::size_t>(fmod_pos(a.y, kChunkDim));
                    const std::size_t oz = static_cast<std::size_t>(fmod_pos(a.z, kChunkDim));
                    if (!src->data[(oz * kChunkDim + oy) * kChunkDim + ox]) continue;
                    if (!any) {
                        lo = hi = c;
                        any = true;
                    } else {
                        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
                        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
                    }
                }
    }
    lv.bounds_lo = lo;
    lv.bounds_hi = hi;
    lv.bounds_empty = !any;
    lv.bounds_valid = true;
}

std::optional<VoxelCoord> VoxelGrid::bounds_min() const {
    ensure_bounds();
    const Level& lv = levels_[active_];
    return lv.bounds_empty ? std::nullopt : std::optional<VoxelCoord>(lv.bounds_lo);
}

std::optional<VoxelCoord> VoxelGrid::bounds_max() const {
    ensure_bounds();
    const Level& lv = levels_[active_];
    return lv.bounds_empty ? std::nullopt : std::optional<VoxelCoord>(lv.bounds_hi);
}

std::optional<VoxelCoord> VoxelGrid::build_plane_pick(const math::Ray& ray,
                                                      std::int32_t plane_cell) const {
    const float vs = voxel_size();
    float plane_y = static_cast<float>(plane_cell) * vs;
    if (kernel::cabs(ray.dir.y) < 1e-9f) return std::nullopt;
    float t = (plane_y - ray.origin.y) / ray.dir.y;
    if (t < 0.0f) return std::nullopt;
    kernel::cfloat3 p = ray.at(t);
    return VoxelCoord{static_cast<std::int32_t>(std::floor(p.x / vs)), plane_cell,
                      static_cast<std::int32_t>(std::floor(p.z / vs))};
}

std::vector<VoxelCoord> VoxelGrid::flood_select(VoxelCoord seed, bool same_color,
                                                std::size_t max_count) const {
    std::vector<VoxelCoord> out;
    std::uint8_t target = get(seed);
    if (target == 0) return out;
    std::unordered_map<VoxelCoord, bool, VoxelCoordHash> seen;
    std::deque<VoxelCoord> queue{seed};
    seen[seed] = true;
    const VoxelCoord dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (!queue.empty() && out.size() < max_count) {
        VoxelCoord c = queue.front();
        queue.pop_front();
        out.push_back(c);
        for (const VoxelCoord& d : dirs) {
            VoxelCoord n{c.x + d.x, c.y + d.y, c.z + d.z};
            if (seen.count(n)) continue;
            std::uint8_t v = get(n);
            if (v == 0 || (same_color && v != target)) continue;
            seen[n] = true;
            queue.push_back(n);
        }
    }
    return out;
}

// -- serialization -----------------------------------------------------------

namespace {
// Guard for the level tail. A reader that predates it stops at the end of the
// coarsest level's chunks and never looks, which is what makes an older build
// open a multi-resolution document at the coarsest level rather than fail; a
// reader that does look needs the tag to tell a tail from a truncated file.
constexpr std::uint32_t kLevelTail = 0x564C4343u;   // "CCLV" little-endian
// A tail that also carries each level's refined-chunk set. A separate tag
// rather than a field on the old one, so a grid whose levels are all whole
// still writes the exact bytes it always did — and a reader predating regions
// meets an unknown tag, which it already handles by opening the grid at its
// coarsest level rather than by failing.
constexpr std::uint32_t kRegionTail = 0x56524343u;  // "CCRV"
// Sculpt layers ride after the level tail, tagged, so a grid with none writes
// the exact bytes it always did and a reader predating them meets an unknown
// tag — which it already handles by opening the grid FLATTENED, which is the
// honest degradation: the sculpt is what the layers composed to.
constexpr std::uint32_t kSculptTail = 0x534C4343u;  // "CCLS"

}  // namespace

// The sculpt-layer tail: nothing at all when there are no layers, so a grid
// that does not use them pays not one byte for the feature existing.
void VoxelGrid::write_sculpt_tail(std::vector<std::uint8_t>* out) const {
    if (sculpt_layers_.empty()) return;
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out->push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
    };
    auto putf = [&](float f) {
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        put32(bits);
    };
    put32(kSculptTail);
    put32(static_cast<std::uint32_t>(sculpt_layers_.size()));
    for (const SculptLayerRecord& rec : sculpt_layers_) {
        put32(static_cast<std::uint32_t>(rec.name.size()));
        for (char ch : rec.name) out->push_back(static_cast<std::uint8_t>(ch));
        putf(rec.strength);
        put32(rec.visible ? 1u : 0u);
        put32(rec.seed);
        put32(static_cast<std::uint32_t>(rec.changes.size()));
        // In the order the pass made them, because that is the order a replay
        // has to use — this is a sequence, not a set.
        for (const SculptChange& ch : rec.changes) {
            put32(static_cast<std::uint32_t>(ch.cell.x));
            put32(static_cast<std::uint32_t>(ch.cell.y));
            put32(static_cast<std::uint32_t>(ch.cell.z));
            out->push_back(ch.before);
            out->push_back(ch.after);
        }
    }
}

bool VoxelGrid::read_sculpt_tail(const std::uint8_t* data, std::size_t size, std::size_t* pos) {
    auto get32 = [&](std::uint32_t* v) {
        if (*pos + 4 > size) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) *v |= static_cast<std::uint32_t>(data[(*pos)++]) << (i * 8);
        return true;
    };
    std::uint32_t count = 0;
    if (!get32(&count)) return false;
    if (count > (size - *pos) / 20) return false;  // smallest possible encoded layer
    for (std::uint32_t i = 0; i < count; ++i) {
        SculptLayerRecord rec;
        std::uint32_t n = 0;
        if (!get32(&n) || n > size - *pos) return false;
        rec.name.assign(reinterpret_cast<const char*>(data + *pos), n);
        *pos += n;
        std::uint32_t bits = 0, vis = 0, changes = 0;
        if (!get32(&bits)) return false;
        std::memcpy(&rec.strength, &bits, 4);
        if (!get32(&vis) || !get32(&rec.seed) || !get32(&changes)) return false;
        rec.visible = vis != 0;
        if (changes > (size - *pos) / 14) return false;  // 14 bytes per change
        rec.changes.reserve(changes);
        for (std::uint32_t c = 0; c < changes; ++c) {
            std::uint32_t x, y, z;
            if (!get32(&x) || !get32(&y) || !get32(&z) || *pos + 2 > size) return false;
            SculptChange ch;
            ch.cell = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                       static_cast<std::int32_t>(z)};
            ch.before = data[(*pos)++];
            ch.after = data[(*pos)++];
            rec.index.emplace(ch.cell, rec.changes.size());
            rec.changes.push_back(ch);
        }
        next_sculpt_seed_ = std::max(next_sculpt_seed_, rec.seed + 1);
        sculpt_layers_.push_back(std::move(rec));
    }
    return true;
}

namespace {
// Ceiling on the cells a tail may ask the reader to MATERIALISE. kMaxLevels
// bounds how many levels a stream can name but not what naming them costs:
// every level is rebuilt by subdividing the one below, so a fixed-size tail
// asks for eight times the cells per level it declares. Twelve bytes claiming
// sixteen levels over a four-cube is a request for 8^15 cells from a 220-byte
// file, which is why the depth alone is not a bound. The budget is far above
// any sculpt that could have been written — the coarsest level is the small
// one — and far below the point where opening a document stops returning.
constexpr std::size_t kMaxLevelCells = 1u << 26;
}  // namespace

std::vector<std::uint8_t> VoxelGrid::serialize() const {
    std::vector<std::uint8_t> out;
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    };
    auto putf = [&](float f) {
        std::uint32_t v;
        std::memcpy(&v, &f, 4);
        put32(v);
    };
    // The COARSEST level, in the layout this stream has always had.
    putf(levels_.front().voxel_size);
    put32(static_cast<std::uint32_t>(palette_.size()));
    for (const cfloat3& c : palette_) {
        putf(c.x);
        putf(c.y);
        putf(c.z);
    }
    // deterministic chunk order
    std::map<std::tuple<int, int, int>, const Chunk*> ordered;
    for (const auto& [key, chunk] : levels_.front().chunks) ordered[{key.x, key.y, key.z}] = &chunk;
    put32(static_cast<std::uint32_t>(ordered.size()));
    for (const auto& [key, chunk] : ordered) {
        put32(static_cast<std::uint32_t>(std::get<0>(key)));
        put32(static_cast<std::uint32_t>(std::get<1>(key)));
        put32(static_cast<std::uint32_t>(std::get<2>(key)));
        // RLE: (value, run) pairs over the linear chunk data
        std::vector<std::uint8_t> rle;
        const std::vector<std::uint8_t>& d = chunk->data;
        for (std::size_t i = 0; i < d.size();) {
            std::uint8_t v = d[i];
            std::size_t run = 1;
            while (i + run < d.size() && d[i + run] == v && run < 0xFFFF) ++run;
            rle.push_back(v);
            rle.push_back(static_cast<std::uint8_t>(run & 0xFF));
            rle.push_back(static_cast<std::uint8_t>(run >> 8));
            i += run;
        }
        put32(static_cast<std::uint32_t>(rle.size()));
        out.insert(out.end(), rle.begin(), rle.end());
    }
    if (levels_.size() == 1) {
        // Byte-for-byte the stream it always was — plus the sculpt tail, which
        // is itself absent when there are no layers.
        write_sculpt_tail(&out);
        return out;
    }

    // Finer levels ride along as their OFFSETS ONLY — the cells that differ
    // from the level below — because everything else is reproducible by
    // subdividing. A level carrying no detail therefore costs four bytes, which
    // is the answer to whether a stack has to multiply the file size.
    bool any_partial = false;
    for (const Level& lv : levels_) any_partial = any_partial || !lv.whole;
    put32(any_partial ? kRegionTail : kLevelTail);
    put32(static_cast<std::uint32_t>(levels_.size()));
    put32(static_cast<std::uint32_t>(active_));
    for (std::size_t i = 1; i < levels_.size(); ++i) {
        if (any_partial) {
            // Ordered, like the detail below it, so the stream stays a
            // canonical form of the grid rather than a hash-order snapshot.
            std::vector<std::tuple<int, int, int>> keys;
            keys.reserve(levels_[i].refined.size());
            for (const VoxelCoord& k : levels_[i].refined) keys.push_back({k.x, k.y, k.z});
            std::sort(keys.begin(), keys.end());
            // A whole level inside a stack that has partial ones is written as
            // "whole" rather than as every one of its chunk keys.
            put32(levels_[i].whole ? 0xFFFFFFFFu : static_cast<std::uint32_t>(keys.size()));
            if (!levels_[i].whole)
                for (const auto& k : keys) {
                    put32(static_cast<std::uint32_t>(std::get<0>(k)));
                    put32(static_cast<std::uint32_t>(std::get<1>(k)));
                    put32(static_cast<std::uint32_t>(std::get<2>(k)));
                }
        }
        std::map<std::tuple<int, int, int>, std::uint8_t> ordered_detail;
        for (const auto& [c, v] : levels_[i].detail) ordered_detail[{c.x, c.y, c.z}] = v;
        put32(static_cast<std::uint32_t>(ordered_detail.size()));
        for (const auto& [c, v] : ordered_detail) {
            put32(static_cast<std::uint32_t>(std::get<0>(c)));
            put32(static_cast<std::uint32_t>(std::get<1>(c)));
            put32(static_cast<std::uint32_t>(std::get<2>(c)));
            out.push_back(v);
        }
    }
    write_sculpt_tail(&out);
    return out;
}

std::optional<VoxelGrid> VoxelGrid::deserialize(const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 0;
    auto get32 = [&](std::uint32_t* v) {
        if (pos + 4 > size) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) *v |= static_cast<std::uint32_t>(data[pos++]) << (i * 8);
        return true;
    };
    auto getf = [&](float* f) {
        std::uint32_t v;
        if (!get32(&v)) return false;
        std::memcpy(f, &v, 4);
        return true;
    };
    float vs;
    std::uint32_t palette_count;
    if (!getf(&vs) || !get32(&palette_count) || palette_count == 0 || palette_count > 256)
        return std::nullopt;
    // Every world<->cell conversion divides by the voxel size, and the result
    // is cast to int32. A payload carrying zero, a negative or a non-finite
    // size made that cast undefined; MaskField::deserialize already refuses the
    // same way. `!(vs > 0)` also rejects NaN.
    if (!(vs > 0.0f) || !std::isfinite(vs)) return std::nullopt;
    VoxelGrid grid(vs);
    grid.palette_.resize(palette_count, cf3(0, 0, 0));
    for (std::uint32_t i = 0; i < palette_count; ++i)
        if (!getf(&grid.palette_[i].x) || !getf(&grid.palette_[i].y) ||
            !getf(&grid.palette_[i].z))
            return std::nullopt;
    std::uint32_t chunk_count;
    if (!get32(&chunk_count)) return std::nullopt;
    // A chunk costs 16 header bytes plus at least 3 for one run, but decodes to
    // 32 KiB — roughly 1700x. Bounding the count by what the remaining payload
    // could possibly describe keeps that ratio from turning a small file into
    // gigabytes of chunks before the first short read is noticed.
    if (chunk_count > (size - pos) / 19) return std::nullopt;
    const std::size_t chunk_cells = static_cast<std::size_t>(kChunkDim) * kChunkDim * kChunkDim;
    for (std::uint32_t c = 0; c < chunk_count; ++c) {
        std::uint32_t kx, ky, kz, rle_size;
        if (!get32(&kx) || !get32(&ky) || !get32(&kz) || !get32(&rle_size)) return std::nullopt;
        if (pos + rle_size > size || rle_size % 3 != 0) return std::nullopt;
        Chunk chunk;
        chunk.data.assign(chunk_cells, 0);
        std::size_t cell = 0;
        for (std::uint32_t i = 0; i < rle_size; i += 3) {
            std::uint8_t v = data[pos + i];
            std::size_t run = data[pos + i + 1] | (static_cast<std::size_t>(data[pos + i + 2]) << 8);
            if (cell + run > chunk_cells) return std::nullopt;
            std::memset(chunk.data.data() + cell, v, run);
            if (v != 0) chunk.occupied += static_cast<int>(run);
            cell += run;
        }
        pos += rle_size;
        if (cell != chunk_cells) return std::nullopt;
        if (chunk.occupied > 0)
            grid.levels_.front().chunks.emplace(
                VoxelCoord{static_cast<std::int32_t>(kx), static_cast<std::int32_t>(ky),
                           static_cast<std::int32_t>(kz)},
                std::move(chunk));
    }
    // No tail, or a stream written before there was one: a single-level grid,
    // which is what an older document is.
    if (!grid.read_tails(data, size, &pos, palette_count)) return std::nullopt;
    grid.mark_every_chunk_dirty();
    return grid;
}

// Everything after the base voxel stream: the level tail, the sculpt tail, or
// neither. A tag this build does not know is not an error — it is an older
// reader's view of a newer file, and stopping there yields the flattened grid,
// which is what every tail here is an addition to. False means a tail WAS
// recognised and then failed to decode, which is a corrupt file.
bool VoxelGrid::read_tails(const std::uint8_t* data, std::size_t size, std::size_t* pos,
                           std::uint32_t palette_count) {
    auto tag_at = [&](std::uint32_t* out) {
        if (*pos + 4 > size) return false;
        *out = 0;
        for (int i = 0; i < 4; ++i) *out |= static_cast<std::uint32_t>(data[*pos + i]) << (i * 8);
        return true;
    };
    std::uint32_t tag = 0;
    if (!tag_at(&tag)) return true;

    if (tag == kLevelTail || tag == kRegionTail) {
        *pos += 4;
        if (!read_level_tail(data, size, pos, palette_count, tag == kRegionTail)) return false;
        // Sculpt layers ride after the levels, when there are any.
        if (!tag_at(&tag)) return true;
    }
    if (tag != kSculptTail) return true;  // unknown, or nothing more to read
    *pos += 4;
    return read_sculpt_tail(data, size, pos);
}

// A grid read back from a file has never been displayed, so every chunk it
// carries is something a host still has to draw. The coarsest level's chunks
// are placed straight into the map here rather than written cell by cell, so
// they are marked in one pass at the end instead — and the finer levels are
// re-marked with them, which costs one insert per chunk and keeps the rule
// "what a reader must draw" in one place.
void VoxelGrid::mark_every_chunk_dirty() {
    for (Level& lv : levels_) {
        lv.dirty_memo_valid = false;
        for (const auto& [key, chunk] : lv.chunks)
            if (chunk.occupied > 0) lv.dirty.insert(key);
    }
}

bool VoxelGrid::read_level_tail(const std::uint8_t* data, std::size_t size, std::size_t* pos,
                                std::uint32_t palette_count, bool has_regions) {
    auto get32 = [&](std::uint32_t* v) {
        if (*pos + 4 > size) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) *v |= static_cast<std::uint32_t>(data[(*pos)++]) << (i * 8);
        return true;
    };
    std::uint32_t level_count = 0, active = 0;
    if (!get32(&level_count) || !get32(&active)) return false;
    if (level_count < 2 || level_count > kMaxLevels || active >= level_count) return false;
    // What the declared depth will cost, charged against the content the file
    // actually supplied, before a single level is built. Subdivision is exact,
    // so this is the count rather than an estimate of it.
    std::size_t per_level = level_occupied_count(0);
    std::size_t projected = 0;
    for (std::uint32_t i = 1; i < level_count; ++i) {
        if (per_level > (kMaxLevelCells - projected) / kChildren) return false;
        per_level *= kChildren;
        projected += per_level;
    }

    for (std::uint32_t i = 1; i < level_count; ++i) {
        // The refined set comes first, because add_level below has to know
        // which chunks to seed before it seeds any of them.
        std::vector<VoxelCoord> refined;
        bool whole = true;
        if (has_regions) {
            std::uint32_t n = 0;
            if (!get32(&n)) return false;
            if (n != 0xFFFFFFFFu) {
                whole = false;
                if (n > (size - *pos) / 12) return false;  // 12 bytes per key
                refined.reserve(n);
                for (std::uint32_t e = 0; e < n; ++e) {
                    std::uint32_t x, y, z;
                    if (!get32(&x) || !get32(&y) || !get32(&z)) return false;
                    refined.push_back({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                       static_cast<std::int32_t>(z)});
                }
            }
        }
        std::uint32_t detail_count = 0;
        if (!get32(&detail_count)) return false;
        // 13 bytes per entry, so a count the rest of the payload cannot possibly
        // describe is refused before anything is reserved for it.
        if (detail_count > (size - *pos) / 13) return false;
        // Subdividing rebuilds everything the offsets do not override, so only
        // the offsets are on the wire. Levels arrive coarsest first, so no finer
        // level exists yet to replay into and an offset is simply restored —
        // going through set() would average it back DOWN and rewrite the
        // coarser levels the file just supplied.
        if (whole) {
            if (add_level() != i) return false;  // the cap refused; the indices would drift
        } else {
            // Built by hand rather than through add_level(region): the file
            // carries the chunk KEYS, and turning them back into a world box to
            // re-derive them would round twice and could land on a different set.
            if (levels_.size() >= kMaxLevels) return false;
            Level& lv = levels_.emplace_back();
            lv.voxel_size = levels_[levels_.size() - 2].voxel_size * 0.5f;
            lv.whole = false;
            if (levels_.size() - 1 != i) return false;
            for (const VoxelCoord& k : refined) lv.refined.insert(k);
            seed_refined_chunks(i);
        }
        for (std::uint32_t e = 0; e < detail_count; ++e) {
            std::uint32_t x, y, z;
            if (!get32(&x) || !get32(&y) || !get32(&z) || *pos >= size) return false;
            std::uint8_t v = data[(*pos)++];
            if (static_cast<std::uint32_t>(v) >= palette_count) return false;
            VoxelCoord c{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                         static_cast<std::int32_t>(z)};
            write_cell(i, c, v);
            record_detail(i, c);
        }
    }
    set_active_level(active);
    return true;
}

// -- greedy meshing ----------------------------------------------------------

namespace {
// For each of 6 face directions, sweep slices and greedy-merge rectangles of
// equal palette index whose faces are exposed.
struct Dir {
    int axis;  // face normal axis 0/1/2
    int sign;  // +1 or -1
};
constexpr Dir kDirs[6] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, 1}, {2, -1}};

// map (slice a, u, v) back to xyz for a given axis
inline VoxelCoord slice_cell(int a, int u, int v, int axis) {
    VoxelCoord c;
    if (axis == 0) c = {a, u, v};
    if (axis == 1) c = {u, a, v};
    if (axis == 2) c = {u, v, a};
    return c;
}
inline int axis_of(VoxelCoord c, int axis) { return axis == 0 ? c.x : (axis == 1 ? c.y : c.z); }
inline int u_of(VoxelCoord c, int axis) { return axis == 0 ? c.y : c.x; }
inline int v_of(VoxelCoord c, int axis) { return axis == 2 ? c.y : c.z; }

// One slab's (u, v) window, in cells.
struct Slab {
    int u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

// Slab index along the sweep axis -> the (u, v) box its occupied chunks span.
// Templated only so an anonymous-namespace function can take VoxelGrid's
// private chunk map without naming it; both quad meshing and greedy meshing
// sweep the same slabs, and two copies of this would be two surfaces.
void collect_slabs(const std::vector<VoxelCoord>& keys, int axis, std::map<int, Slab>& out) {
    for (const VoxelCoord& key : keys) {
        const int sa = axis_of(key, axis);
        const int cu = u_of(key, axis) * kChunkDim;
        const int cv = v_of(key, axis) * kChunkDim;
        auto it = out.find(sa);
        if (it == out.end()) {
            out.emplace(sa, Slab{cu, cu + kChunkDim - 1, cv, cv + kChunkDim - 1});
            continue;
        }
        it->second.u0 = std::min(it->second.u0, cu);
        it->second.u1 = std::max(it->second.u1, cu + kChunkDim - 1);
        it->second.v0 = std::min(it->second.v0, cv);
        it->second.v1 = std::max(it->second.v1, cv + kChunkDim - 1);
    }
}
}  // namespace

void VoxelGrid::sweep_window(std::size_t level, int axis, int sign, int slab_index, int u0,
                             int v0, int nu, int nv, std::vector<std::uint8_t>& mask,
                             mesh::Mesh& out, FaceWeld* weld) const {
    const ChunkMap& chunks = levels_[level].chunks;
    auto value_at = [&](VoxelCoord c) { return cell_at(level, c); };
    const Dir dir{axis, sign};

    // Build one slice's exposure mask, one CHUNK at a time.
    //
    // The mask window is chunk-aligned by construction (the slab bounds below
    // are chunk corners), and within a slice every cell of a chunk column
    // shares one chunk key. So the map is probed once per chunk instead of once
    // per cell: `cell_at` is a hash plus an unordered_map::find, and the sweep
    // touches 6 directions x 32 slices x the whole 32x32 window per chunk —
    // ~200K finds per chunk, which is why a chunk holding one voxel used to
    // cost the same as a full one.
    //
    // Inside the chunk the flat kChunkDim^3 payload is addressed as
    // base + u*du + v*dv, the strides being whichever of x/y/z the sweep axis
    // maps u and v onto. The only probe that can leave the chunk is the
    // neighbour across the face, and only on the slice sitting on the chunk's
    // own boundary; that one still goes through `cell_at`.
    //
    // Nothing is written for a chunk that does not exist or a cell that is
    // empty. The merge below zeroes every entry it consumes and leaves the
    // zeros it skipped, so the mask is all-zero again at the top of each slice
    // — which is what keeps a slab window spanning mostly empty space cheap.
    auto build_slice_mask = [&](int a) {
        constexpr std::size_t kCd = static_cast<std::size_t>(kChunkDim);
        const std::size_t du = dir.axis == 0 ? kCd : 1;
        const std::size_t dv = dir.axis == 2 ? kCd : kCd * kCd;
        const std::size_t da = dir.axis == 0 ? 1 : (dir.axis == 1 ? kCd : kCd * kCd);
        const int la = fmod_pos(a, kChunkDim);
        const int la_n = la + dir.sign;
        const bool neighbour_inside = la_n >= 0 && la_n < kChunkDim;
        const std::size_t base = static_cast<std::size_t>(la) * da;
        const std::size_t nbase =
            neighbour_inside ? static_cast<std::size_t>(la_n) * da : std::size_t{0};

        const int ku0 = fdiv(u0, kChunkDim), kv0 = fdiv(v0, kChunkDim);
        for (int kv = 0; kv * kChunkDim < nv; ++kv)
            for (int ku = 0; ku * kChunkDim < nu; ++ku) {
                auto it = chunks.find(slice_cell(slab_index, ku0 + ku, kv0 + kv, dir.axis));
                if (it == chunks.end()) continue;  // no chunk here: the mask is already 0
                const std::uint8_t* data = it->second.data.data();
                for (int lv = 0; lv < kChunkDim; ++lv) {
                    std::uint8_t* row =
                        &mask[static_cast<std::size_t>(kv * kChunkDim + lv) * nu + ku * kChunkDim];
                    const std::size_t off = base + static_cast<std::size_t>(lv) * dv;
                    const std::size_t noff = nbase + static_cast<std::size_t>(lv) * dv;
                    for (int lu = 0; lu < kChunkDim; ++lu) {
                        const std::size_t step = static_cast<std::size_t>(lu) * du;
                        std::uint8_t idx = data[off + step];
                        if (idx == 0) continue;  // empty cell: the mask is already 0
                        if (neighbour_inside) {
                            if (data[noff + step] != 0) continue;  // covered face
                        } else {
                            VoxelCoord n = slice_cell(a + dir.sign, u0 + ku * kChunkDim + lu,
                                                      v0 + kv * kChunkDim + lv, dir.axis);
                            if (value_at(n) != 0) continue;  // covered face, across the seam
                        }
                        row[lu] = idx;
                    }
                }
            }
    };

    // Every caller's window is a whole number of chunks — slab bounds are chunk
    // corners, and a single key's window is the chunk itself. build_slice_mask
    // writes a chunk-wide row at a time and steps ku while ku * kChunkDim < nu,
    // so a window that is not chunk-aligned would run a row off the end of the
    // mask — a heap write, not a wrong quad. Tightening a window to the
    // occupied bounding box is the natural next optimisation and is exactly
    // what would break it, so the invariant is asserted rather than described.
    assert(nu % kChunkDim == 0 && nv % kChunkDim == 0);
    mask.assign(static_cast<std::size_t>(nu) * nv, 0);
    const int a0 = slab_index * kChunkDim;
    const int a1 = a0 + kChunkDim - 1;

    for (int a = a0; a <= a1; ++a) {
        build_slice_mask(a);
        // Greedy merge. It also hands the mask back all-zero — every entry
        // it reads nonzero it zeroes — which is the precondition the mask
        // build relies on to skip empty chunks and empty cells entirely.
        for (int v = 0; v < nv; ++v)
            for (int u = 0; u < nu;) {
                std::uint8_t idx = mask[static_cast<std::size_t>(v) * nu + u];
                if (idx == 0) {
                    ++u;
                    continue;
                }
                // Faces mode takes the run as it is — one quad per exposed
                // face — so the merge is skipped rather than undone. A merged
                // rectangle abutting several shorter ones is a T-junction by
                // construction, which cracks under subdivision, and that is
                // why the quad path does not merge.
                int w = 1, h = 1;
                if (weld != nullptr) {
                    emit_face_quad(out, *weld, dir.axis, dir.sign, a, u + u0, v + v0, idx,
                                   levels_[level].voxel_size);
                } else {
                    while (u + w < nu && mask[static_cast<std::size_t>(v) * nu + u + w] == idx)
                        ++w;
                    bool grow = true;
                    while (grow && v + h < nv) {
                        for (int k = 0; k < w; ++k)
                            if (mask[static_cast<std::size_t>(v + h) * nu + u + k] != idx) {
                                grow = false;
                                break;
                            }
                        if (grow) ++h;
                    }
                    emit_quad(out, dir.axis, dir.sign, a, u + u0, v + v0, w, h, idx,
                              levels_[level].voxel_size);
                }
                for (int dv = 0; dv < h; ++dv)
                    for (int du = 0; du < w; ++du)
                        mask[static_cast<std::size_t>(v + dv) * nu + u + du] = 0;
                u += w;
            }
    }
}

mesh::Mesh VoxelGrid::mesh_greedy(std::size_t level) const {
    mesh::Mesh out;
    if (level >= levels_.size()) return out;
    const std::vector<VoxelCoord> keys = material_chunk_keys(level);
    if (keys.empty()) return out;

    // The sweep runs per occupied CHUNK SLAB rather than over the whole
    // occupied bounding box. A grid is sparse by construction, and a bounding
    // box is not: two voxels far apart on two axes made the box — and the
    // per-slice mask sized from it — enormous, which cost cubic time and, from
    // a payload whose chunk keys are simply far apart, overflowed the int in
    // `u1 - u0 + 1` before allocating a mask no allocator could satisfy.
    //
    // Grouping by slab is exact rather than an approximation: a slice belongs
    // to exactly ONE slab along the sweep axis, so every occupied cell in that
    // slice lies in that slab's (u, v) extent. Cells outside it are empty and
    // emit nothing, so the merge sees the same mask contents and produces the
    // same quads it always did.
    //
    // The window spans the slab's chunks, which is why the merge here CROSSES
    // chunk boundaries — the tighter merge, and the reason this call stays the
    // export path while mesh_greedy_chunks clamps to one chunk.
    std::vector<std::uint8_t> mask;
    for (const Dir& dir : kDirs) {
        std::map<int, Slab> slabs;
        collect_slabs(keys, dir.axis, slabs);
        for (const auto& [slab_index, slab] : slabs)
            sweep_window(level, dir.axis, dir.sign, slab_index, slab.u0, slab.v0,
                         slab.u1 - slab.u0 + 1, slab.v1 - slab.v0 + 1, mask, out);
    }
    return out;
}

std::vector<VoxelCoord> VoxelGrid::occupied_chunk_keys(std::size_t level) const {
    return material_chunk_keys(level);
}

// Per chunk, the same sweep over a window of exactly one chunk. The exposure
// test still reads the neighbour cell through `cell_at`, so an unrequested —
// or absent — neighbour hides a face exactly as it does in the whole-grid
// sweep, and the surface is the same one. Only the merge is clamped: the
// window is 32x32 rather than the slab's, so a quad that spanned a chunk
// boundary comes back as one quad per chunk. More triangles, same surface, no
// crack, and every vertex belongs to exactly one key — which is what makes the
// ranges a partition.
mesh::Mesh VoxelGrid::mesh_greedy_chunks(std::size_t level, const std::vector<VoxelCoord>& keys,
                                         std::vector<VoxelChunkMeshRange>* out_ranges) const {
    mesh::Mesh out;
    if (out_ranges) {
        out_ranges->clear();
        out_ranges->reserve(keys.size());
    }
    const bool have_level = level < levels_.size();
    std::vector<std::uint8_t> mask;
    for (VoxelCoord key : keys) {
        VoxelChunkMeshRange range;
        range.key = key;
        range.vertex_first = static_cast<std::uint32_t>(out.positions.size());
        range.index_first = static_cast<std::uint32_t>(out.indices.size());
        // A key holding no chunk contributes nothing: a drained set names the
        // chunks a stroke emptied as readily as the ones it filled.
        const bool occupied =
            have_level && levels_[level].chunks.find(key) != levels_[level].chunks.end();
        if (occupied)
            for (const Dir& dir : kDirs)
                sweep_window(level, dir.axis, dir.sign, axis_of(key, dir.axis),
                             u_of(key, dir.axis) * kChunkDim, v_of(key, dir.axis) * kChunkDim,
                             kChunkDim, kChunkDim, mask, out);
        range.vertex_count = static_cast<std::uint32_t>(out.positions.size()) - range.vertex_first;
        range.index_count = static_cast<std::uint32_t>(out.indices.size()) - range.index_first;
        if (out_ranges) out_ranges->push_back(range);
    }
    return out;
}

// -- quad meshing ------------------------------------------------------------

// Faces mode's vertex table. Keyed by lattice corner AND palette index: a
// corner shared by faces of one colour becomes ONE vertex, and the corner on a
// colour boundary appears once per colour. That split is what keeps per-face
// palette colour — the reason emit_quad duplicates its four vertices in the
// first place — while everything within a colour region comes out as a
// connected quad grid instead of the soup of disconnected rectangles a greedy
// mesh arrives in a DCC as.
struct VoxelGrid::FaceWeld {
    struct Key {
        std::int32_t x, y, z;
        std::uint8_t idx;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& k) const {
            std::uint64_t h = static_cast<std::uint32_t>(k.x) * 0x9E3779B185EBCA87ull;
            h ^= static_cast<std::uint32_t>(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6);
            h ^= static_cast<std::uint32_t>(k.z) * 0x165667B19E3779F9ull + (h >> 3);
            h ^= static_cast<std::uint64_t>(k.idx) * 0x27D4EB2F165667C5ull + (h << 2);
            return static_cast<std::size_t>(h);
        }
    };
    std::unordered_map<Key, std::uint32_t, Hash> vertex;
};

// One planar, axis-aligned quad for one exposed face, through the weld table.
//
// No vertex normal is written, and that is forced rather than forgotten: a
// welded corner is shared by faces pointing three different ways, so it has no
// single normal — averaging would round the cube the model IS, and duplicating
// the vertex to carry one would undo the weld. The quads are planar, so a
// consumer derives the face normal from the face. A caller who needs per-face
// normals uses mesh_greedy and gets triangles, exactly as before.
void VoxelGrid::emit_face_quad(mesh::Mesh& out, FaceWeld& weld, int axis, int sign, int a, int u,
                               int v, std::uint8_t idx, float cell_size) const {
    const int face = a + (sign > 0 ? 1 : 0);
    const cfloat3 color = palette_color(idx);
    auto corner_index = [&](int du, int dv) {
        FaceWeld::Key key{0, 0, 0, idx};
        const int uu = u + du, vv = v + dv;
        if (axis == 0) key = {face, uu, vv, idx};
        if (axis == 1) key = {uu, face, vv, idx};
        if (axis == 2) key = {uu, vv, face, idx};
        auto it = weld.vertex.find(key);
        if (it != weld.vertex.end()) return it->second;
        const std::uint32_t index = static_cast<std::uint32_t>(out.positions.size());
        weld.vertex.emplace(key, index);
        out.positions.push_back(cf3(static_cast<float>(key.x), static_cast<float>(key.y),
                                    static_cast<float>(key.z)) *
                                cell_size);
        out.colors.push_back(color);
        return index;
    };

    // The corner ORDER carries the winding, so it follows emit_quad's per-axis
    // flip exactly. Get it wrong and the quads and their triangulation
    // disagree about which way the face points — invisible until someone
    // imports with backface culling on.
    const bool flip = axis == 1 ? sign > 0 : sign < 0;
    std::uint32_t q[4] = {corner_index(0, 0), corner_index(1, 0), corner_index(1, 1),
                          corner_index(0, 1)};
    if (flip) std::swap(q[1], q[3]);
    out.quads.insert(out.quads.end(), q, q + 4);
    const std::uint32_t tri[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
    out.indices.insert(out.indices.end(), tri, tri + 6);
}

mesh::Mesh VoxelGrid::mesh_quads(const QuadOptions& options) const {
    if (options.mode == QuadOptions::Mode::Dual)
        return dual_mesh(options.level, options.blur, options.cell_size, /*keep_quads=*/true);

    mesh::Mesh out;
    const std::size_t level = options.level;
    if (level >= levels_.size()) return out;
    const std::vector<VoxelCoord> keys = material_chunk_keys(level);
    if (keys.empty()) return out;

    // The same sweep mesh_greedy runs, over the same slabs, with the merge
    // switched off. Sharing it is the point: the exposure test — and therefore
    // the SURFACE — is mesh_greedy's, so the two cannot disagree about which
    // faces exist.
    FaceWeld weld;
    std::vector<std::uint8_t> mask;
    for (const Dir& dir : kDirs) {
        std::map<int, Slab> slabs;
        collect_slabs(keys, dir.axis, slabs);
        for (const auto& [slab_index, slab] : slabs)
            sweep_window(level, dir.axis, dir.sign, slab_index, slab.u0, slab.v0,
                         slab.u1 - slab.u0 + 1, slab.v1 - slab.v0 + 1, mask, out, &weld);
    }
    return out;
}

namespace {

// A seed cell size for a target count, from the cells a level holds rather
// than from a mesh nobody has built yet. A solid of N cubes exposes on the
// order of 6*N^(2/3) faces, the count is area / cell^2, and the search
// corrects from there — this only has to be the right order of magnitude, and
// a hollow shell (all faces exposed) is the case it underestimates.
float seed_cell_for(std::size_t occupied, float voxel_size, std::size_t target) {
    if (occupied == 0 || target == 0) return voxel_size;
    const double faces = 6.0 * std::cbrt(static_cast<double>(occupied) *
                                         static_cast<double>(occupied));
    const double area = faces * static_cast<double>(voxel_size) * voxel_size;
    return static_cast<float>(std::sqrt(area / static_cast<double>(target)));
}

}  // namespace

// The dual's lever is the cell size, between the level's own voxel size — the
// clamp mesh_quads already applies, because a finer lattice resamples the same
// step field — and a lattice one cell across the whole sculpt.
mesh::Mesh VoxelGrid::dual_quads_fit(const QuadOptions& options, const mesh::QuadTarget& target,
                                     mesh::QuadFit* fit) const {
    mesh::Mesh out;
    if (options.level >= levels_.size()) return out;
    const float vs = levels_[options.level].voxel_size;

    // The coarse limit, taken from the chunk KEYS rather than from the
    // occupied cells: it only has to bound the search, and walking every voxel
    // to bound it would cost more than the extra iteration it might save.
    float span = vs;
    const std::vector<VoxelCoord> span_keys = material_chunk_keys(options.level);
    if (!span_keys.empty()) {
        VoxelCoord lo = span_keys.front(), hi = lo;
        for (const VoxelCoord& key : span_keys) {
            lo = {std::min(lo.x, key.x), std::min(lo.y, key.y), std::min(lo.z, key.z)};
            hi = {std::max(hi.x, key.x), std::max(hi.y, key.y), std::max(hi.z, key.z)};
        }
        const int cells = (std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) + 1) * kChunkDim;
        span = std::max(vs, static_cast<float>(cells) * vs);
    }

    const float floor_cell = (std::isfinite(target.min_cell_size) && target.min_cell_size > vs)
                                 ? std::min(target.min_cell_size, span)
                                 : vs;
    const float seed = (std::isfinite(options.cell_size) && options.cell_size > 0.0f)
                           ? options.cell_size
                           : seed_cell_for(level_occupied_count(options.level), vs, target.target);
    *fit = mesh::fit_quad_cell(
        [&](float cell) { return dual_mesh(options.level, options.blur, cell, true); }, seed,
        floor_cell, span, target, &out);
    return out;
}

// Faces mode has no cell size to step, so the target search is the LADDER walk
// over the level stack: coarsest level first, stopping at the first level that
// reaches the target. mesh/quad_mesh.h holds the contract, including what
// `clamped` means when the lattices are a fixed list — the stack ran out, not
// a limit was hit.
mesh::Mesh VoxelGrid::faces_quads_fit(const QuadOptions& options, const mesh::QuadTarget& target,
                                      mesh::QuadFit* fit) const {
    mesh::Mesh out;
    *fit = mesh::QuadFit{};
    if (levels_.empty()) return out;

    auto mesh_level = [&](std::size_t level) {
        QuadOptions at = options;
        at.level = level;
        return mesh_quads(at);
    };

    if (target.target == 0) {
        // No search, so the caller's own level stands and the report is here
        // only to echo which lattice that was. A level the grid does not have
        // comes back empty and zeroed rather than clamped onto the finest:
        // with no target this call IS mesh_quads with a report attached, and
        // mesh_quads answers an out-of-range level with an empty mesh, as does
        // dual mode. Clamping here would hand a caller who passed a stale
        // level a full-resolution mesh and a report saying it was asked for.
        const std::size_t level = options.level;
        if (level >= levels_.size()) return out;
        out = mesh_level(level);
        fit->cell_size = levels_[level].voxel_size;
        fit->quad_count = out.quad_count();
        fit->within_tolerance = true;
        return out;
    }

    std::size_t rung = 0;
    *fit = mesh::fit_quad_ladder(mesh_level, levels_.size(), target, &rung, &out);
    // The one field the ladder cannot fill: it walked rungs, and what names a
    // rung back to a caller is the level's voxel size.
    fit->cell_size = levels_[rung].voxel_size;
    return out;
}

mesh::Mesh VoxelGrid::mesh_quads_fit(const QuadOptions& options, const mesh::QuadTarget& target,
                                     mesh::QuadFit* out_fit) const {
    mesh::QuadFit fit;
    mesh::Mesh out = options.mode == QuadOptions::Mode::Dual
                         ? dual_quads_fit(options, target, &fit)
                         : faces_quads_fit(options, target, &fit);
    if (out_fit) *out_fit = fit;
    return out;
}

void VoxelGrid::emit_quad(mesh::Mesh& out, int axis, int sign, int a, int u, int v, int w, int h,
                          std::uint8_t idx, float cell_size) const {
    float s = cell_size;
    float face = static_cast<float>(a) + (sign > 0 ? 1.0f : 0.0f);
    auto corner = [&](float uu, float vv) {
        if (axis == 0) return cf3(face, uu, vv) * s;
        if (axis == 1) return cf3(uu, face, vv) * s;
        return cf3(uu, vv, face) * s;
    };
    float uf = static_cast<float>(u), vf = static_cast<float>(v);
    cfloat3 p0 = corner(uf, vf);
    cfloat3 p1 = corner(uf + w, vf);
    cfloat3 p2 = corner(uf + w, vf + h);
    cfloat3 p3 = corner(uf, vf + h);
    cfloat3 n = cf3(0, 0, 0);
    if (axis == 0) n.x = static_cast<float>(sign);
    if (axis == 1) n.y = static_cast<float>(sign);
    if (axis == 2) n.z = static_cast<float>(sign);
    cfloat3 color = palette_color(idx);

    std::uint32_t base = static_cast<std::uint32_t>(out.positions.size());
    for (const cfloat3& p : {p0, p1, p2, p3}) {
        out.positions.push_back(p);
        out.normals.push_back(n);
        out.colors.push_back(color);
    }
    // winding so the face normal points along `n`
    bool flip;
    if (axis == 0)
        flip = sign < 0;
    else if (axis == 1)
        flip = sign > 0;
    else
        flip = sign < 0;
    std::uint32_t tri[6] = {0, 1, 2, 0, 2, 3};
    if (flip) {
        std::swap(tri[1], tri[2]);
        std::swap(tri[4], tri[5]);
    }
    for (std::uint32_t i : tri) out.indices.push_back(base + i);
}

// -- voxel <-> SDF bridges ---------------------------------------------------

float VoxelGrid::sample_step_field(cfloat3 world_p) const {
    const float vs = voxel_size();
    VoxelCoord c{static_cast<std::int32_t>(std::floor(world_p.x / vs)),
                 static_cast<std::int32_t>(std::floor(world_p.y / vs)),
                 static_cast<std::int32_t>(std::floor(world_p.z / vs))};
    return get(c) != 0 ? -0.5f * vs : 0.5f * vs;
}

namespace {

// The colour a cell takes from a mesh that carries vertex colours: the nearest
// triangle's, interpolated at the closest point. Reading it from the CLOSEST
// POINT rather than the nearest vertex is what keeps a large triangle's
// interior from taking one corner's colour.
kernel::cfloat3 mesh_colour_at(const mesh::Mesh& m, const mesh::Bvh& bvh, kernel::cfloat3 p,
                               kernel::cfloat3 fallback) {
    const mesh::Bvh::ClosestPoint hit = bvh.closest(p);
    if (!hit.found) return fallback;
    // The same barycentric read attribute transfer uses. Shared rather than
    // duplicated so the two cannot drift apart — this one is how an imported
    // model's vertex colours reach a palette, and that one is how they survive
    // a trip through the field.
    return mesh::sample_color(m, hit.triangle, hit.u, hit.v, fallback);
}

}  // namespace

void VoxelGrid::rasterize_mesh(const mesh::Mesh& m) {
    if (m.empty() || m.positions.empty()) return;
    math::Aabb bounds;
    for (const kernel::cfloat3& p : m.positions) bounds.expand(p);
    rasterize_mesh(m, bounds);
}

void VoxelGrid::rasterize_mesh(const mesh::Mesh& m, const math::Aabb& world_region) {
    if (m.empty() || world_region.empty() || world_region.is_infinite()) return;
    const mesh::Bvh bvh = mesh::Bvh::build(m);
    if (bvh.empty()) return;  // every triangle had a bad index

    // A mesh with no colours still has to land somewhere in the palette, and a
    // grid's colour is per cell: there is nothing to read, so one neutral entry
    // is the honest answer rather than a colour invented per cell.
    const kernel::cfloat3 neutral = cf3(0.8f, 0.8f, 0.8f);

    const float vs = voxel_size();
    std::int32_t x0 = static_cast<std::int32_t>(std::floor(world_region.min.x / vs));
    std::int32_t y0 = static_cast<std::int32_t>(std::floor(world_region.min.y / vs));
    std::int32_t z0 = static_cast<std::int32_t>(std::floor(world_region.min.z / vs));
    std::int32_t x1 = static_cast<std::int32_t>(std::floor(world_region.max.x / vs));
    std::int32_t y1 = static_cast<std::int32_t>(std::floor(world_region.max.y / vs));
    std::int32_t z1 = static_cast<std::int32_t>(std::floor(world_region.max.z / vs));
    // The same two-phase split rasterize_tape uses, for the same reason: the
    // winding number and the closest-point query are pure reads of a const BVH,
    // and the palette insert is not.
    struct Hit {
        std::int32_t x, y, z;
        kernel::cfloat3 color;
    };
    constexpr std::int32_t kPlanesPerWave = 32;
    if (x1 < x0 || y1 < y0) return;

    std::vector<std::vector<Hit>> hits(static_cast<std::size_t>(kPlanesPerWave));
    for (std::int32_t wave = z0; wave <= z1; wave += kPlanesPerWave) {
        const std::int32_t wave_end = std::min<std::int32_t>(wave + kPlanesPerWave - 1, z1);
        const std::size_t planes = static_cast<std::size_t>(wave_end - wave + 1);
        for (std::size_t i = 0; i < planes; ++i) hits[i].clear();

        parallel::for_range(planes, 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) {
                const std::int32_t z = wave + static_cast<std::int32_t>(i);
                std::vector<Hit>& out = hits[i];
                for (std::int32_t y = y0; y <= y1; ++y)
                    for (std::int32_t x = x0; x <= x1; ++x) {
                        cfloat3 center = cf3((static_cast<float>(x) + 0.5f) * vs,
                                             (static_cast<float>(y) + 0.5f) * vs,
                                             (static_cast<float>(z) + 0.5f) * vs);
                        if (!bvh.is_inside(center)) continue;
                        // The closest-point query only runs for a cell that is
                        // IN: it is the expensive half and most cells are not.
                        out.push_back({x, y, z, mesh_colour_at(m, bvh, center, neutral)});
                    }
            }
        });

        for (std::size_t i = 0; i < planes; ++i)
            for (const Hit& h : hits[i])
                set({h.x, h.y, h.z}, palette_add(h.color, 1.0f / 64.0f));
    }
}

void VoxelGrid::rasterize_tape(const scene::Tape& tape, const math::Aabb& world_region) {
    if (world_region.empty() || world_region.is_infinite()) return;
    const float vs = voxel_size();
    std::int32_t x0 = static_cast<std::int32_t>(std::floor(world_region.min.x / vs));
    std::int32_t y0 = static_cast<std::int32_t>(std::floor(world_region.min.y / vs));
    std::int32_t z0 = static_cast<std::int32_t>(std::floor(world_region.min.z / vs));
    std::int32_t x1 = static_cast<std::int32_t>(std::floor(world_region.max.x / vs));
    std::int32_t y1 = static_cast<std::int32_t>(std::floor(world_region.max.y / vs));
    std::int32_t z1 = static_cast<std::int32_t>(std::floor(world_region.max.z / vs));
    // EVALUATE in parallel, WRITE serially — the same two-phase split the
    // sculpting verbs use, and for the same two reasons.
    //
    // A compiled tape is const during eval and the kernels hold no state, so
    // any number of threads can hammer one; the C ABI already promises that of
    // `clay_brick_cache_eval_requests`. What cannot run concurrently is the
    // WRITING: `set` mutates a chunk map and `palette_add` inserts a
    // nearest-entry match into a shared palette.
    //
    // Byte-identity is by construction. Planes are decided into their own
    // buckets and applied in plane order, so the palette sees colours in the
    // order the serial walk saw them and gets the same indices — which matters
    // more here than for a verb, because a palette index is stored.
    //
    // WAVES, so the pending list is bounded by the wave rather than by the
    // region: a document rasterized at a fine cell is millions of cells, and
    // holding a colour for every one of them would trade time for a memory
    // spike. The same shape parallel brick meshing uses for the same reason.
    struct Hit {
        std::int32_t x, y, z;
        cfloat3 color;
    };
    constexpr std::int32_t kPlanesPerWave = 32;
    const std::int32_t nx = x1 - x0 + 1, ny = y1 - y0 + 1;
    if (nx <= 0 || ny <= 0) return;

    std::vector<std::vector<Hit>> hits(static_cast<std::size_t>(kPlanesPerWave));
    for (std::int32_t wave = z0; wave <= z1; wave += kPlanesPerWave) {
        const std::int32_t wave_end = std::min<std::int32_t>(wave + kPlanesPerWave - 1, z1);
        const std::size_t planes = static_cast<std::size_t>(wave_end - wave + 1);
        for (std::size_t i = 0; i < planes; ++i) hits[i].clear();

        parallel::for_range(planes, 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) {
                const std::int32_t z = wave + static_cast<std::int32_t>(i);
                std::vector<Hit>& out = hits[i];
                for (std::int32_t y = y0; y <= y1; ++y)
                    for (std::int32_t x = x0; x <= x1; ++x) {
                        cfloat3 center = cf3((static_cast<float>(x) + 0.5f) * vs,
                                             (static_cast<float>(y) + 0.5f) * vs,
                                             (static_cast<float>(z) + 0.5f) * vs);
                        kernel::CTapeValue v = tape.eval(center);
                        if (v.d < 0.0f) out.push_back({x, y, z, v.color});
                    }
            }
        });

        for (std::size_t i = 0; i < planes; ++i)
            for (const Hit& h : hits[i])
                set({h.x, h.y, h.z}, palette_add(h.color, 1.0f / 64.0f));
    }
}

}  // namespace voxel
}  // namespace clay
