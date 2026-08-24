#include "clay/voxel/groups.h"

#include <algorithm>
#include <cmath>

namespace clay {
namespace voxel {

using kernel::cfloat3;

namespace {

// Floor division and positive modulo, so a lattice addressed with negative
// coordinates chunks the same way it does with positive ones. Same shapes
// MaskField uses; a group field is deliberately the same lattice.
std::int32_t fdiv(std::int32_t a, std::int32_t b) {
    return a >= 0 ? a / b : -(((-a) + b - 1) / b);
}

std::int32_t fmod_pos(std::int32_t a, std::int32_t b) {
    const std::int32_t m = a % b;
    return m < 0 ? m + b : m;
}

constexpr std::size_t kCells =
    static_cast<std::size_t>(kChunkDim) * kChunkDim * kChunkDim;

}  // namespace

VoxelCoord GroupField::chunk_key(VoxelCoord c) {
    return {fdiv(c.x, kChunkDim), fdiv(c.y, kChunkDim), fdiv(c.z, kChunkDim)};
}

std::size_t GroupField::chunk_offset(VoxelCoord c) {
    const std::int32_t x = fmod_pos(c.x, kChunkDim);
    const std::int32_t y = fmod_pos(c.y, kChunkDim);
    const std::int32_t z = fmod_pos(c.z, kChunkDim);
    return (static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x;
}

VoxelCoord GroupField::cell_at(cfloat3 p) const {
    return {static_cast<std::int32_t>(std::floor(p.x / cell_size_)),
            static_cast<std::int32_t>(std::floor(p.y / cell_size_)),
            static_cast<std::int32_t>(std::floor(p.z / cell_size_))};
}

cfloat3 GroupField::cell_centre(VoxelCoord c) const {
    return kernel::cf3((static_cast<float>(c.x) + 0.5f) * cell_size_,
                       (static_cast<float>(c.y) + 0.5f) * cell_size_,
                       (static_cast<float>(c.z) + 0.5f) * cell_size_);
}

GroupId GroupField::get(VoxelCoord c) const {
    auto it = chunks_.find(chunk_key(c));
    if (it == chunks_.end()) return kNoGroup;
    return it->second.data[chunk_offset(c)];
}

void GroupField::set(VoxelCoord c, GroupId id) {
    touch();
    const VoxelCoord key = chunk_key(c);
    auto it = chunks_.find(key);
    if (it == chunks_.end()) {
        // Storing kNoGroup would only allocate an empty chunk. An unassigned
        // lattice is the common case and must stay free.
        if (id == kNoGroup) return;
        it = chunks_.emplace(key, Chunk{}).first;
        it->second.data.assign(kCells, kNoGroup);
    }
    GroupId& cell = it->second.data[chunk_offset(c)];
    if (cell == kNoGroup && id != kNoGroup) ++it->second.assigned;
    if (cell != kNoGroup && id == kNoGroup) --it->second.assigned;
    cell = id;
    // A chunk that reaches zero goes, because a missing chunk is how this
    // lattice spells "nothing here" — same rule the mask uses.
    if (it->second.assigned == 0) chunks_.erase(it);
}

void GroupField::fill(const math::Aabb& region, GroupId id) {
    if (region.empty()) return;
    const VoxelCoord lo = cell_at(region.min);
    const VoxelCoord hi = cell_at(region.max);
    for (std::int32_t z = lo.z; z <= hi.z; ++z)
        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) {
                const VoxelCoord c{x, y, z};
                // Cell CENTRE, so a region that clips a cell does not claim it.
                // The same rule MaskField::fill uses, and the reason two
                // adjacent fills do not overlap by a cell.
                const cfloat3 p = cell_centre(c);
                if (p.x < region.min.x || p.x > region.max.x) continue;
                if (p.y < region.min.y || p.y > region.max.y) continue;
                if (p.z < region.min.z || p.z > region.max.z) continue;
                set(c, id);
            }
}

std::size_t GroupField::reassign(GroupId from, GroupId to) {
    if (from == to) return 0;
    std::size_t moved = 0;
    touch();
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        Chunk& ch = it->second;
        for (GroupId& v : ch.data) {
            if (v != from) continue;
            if (v == kNoGroup) continue;
            v = to;
            ++moved;
            if (to == kNoGroup) --ch.assigned;
        }
        if (ch.assigned == 0)
            it = chunks_.erase(it);
        else
            ++it;
    }
    // Merging a group away takes its visibility with it: a hidden id nobody
    // carries would keep hiding a group that no longer exists.
    if (to == kNoGroup) hidden_.erase(from);
    return moved;
}

void GroupField::set_visible(GroupId id, bool is_visible) {
    if (id == kNoGroup) return;  // ungrouped surface is not something to hide
    touch();
    if (is_visible)
        hidden_.erase(id);
    else
        hidden_.insert(id);
}

bool GroupField::visible(GroupId id) const {
    if (id == kNoGroup) return true;
    return hidden_.find(id) == hidden_.end();
}

void GroupField::isolate(GroupId id) {
    touch();
    hidden_.clear();
    for (GroupId other : ids())
        if (other != id) hidden_.insert(other);
    // kNoGroup stays visible: ungrouped surface is not something an artist hid,
    // and isolating a group should not make the rest of the model vanish
    // because it was never named.
}

void GroupField::show_all() {
    touch();
    hidden_.clear();
}

std::size_t GroupField::cell_count() const {
    std::size_t n = 0;
    for (const auto& [key, chunk] : chunks_) n += static_cast<std::size_t>(chunk.assigned);
    return n;
}

std::size_t GroupField::cell_count(GroupId id) const {
    if (id == kNoGroup) return 0;
    std::size_t n = 0;
    for (const auto& [key, chunk] : chunks_)
        for (GroupId v : chunk.data)
            if (v == id) ++n;
    return n;
}

std::vector<GroupId> GroupField::ids() const {
    std::vector<GroupId> out;
    for (const auto& [key, chunk] : chunks_)
        for (GroupId v : chunk.data)
            if (v != kNoGroup) out.push_back(v);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<VoxelCoord> GroupField::bounds_min() const {
    std::optional<VoxelCoord> out;
    for (const auto& [key, chunk] : chunks_)
        for (std::size_t i = 0; i < kCells; ++i) {
            if (chunk.data[i] == kNoGroup) continue;
            const VoxelCoord c{
                key.x * kChunkDim + static_cast<std::int32_t>(i % kChunkDim),
                key.y * kChunkDim + static_cast<std::int32_t>((i / kChunkDim) % kChunkDim),
                key.z * kChunkDim +
                    static_cast<std::int32_t>(i / (static_cast<std::size_t>(kChunkDim) * kChunkDim))};
            if (!out) {
                out = c;
            } else {
                out->x = std::min(out->x, c.x);
                out->y = std::min(out->y, c.y);
                out->z = std::min(out->z, c.z);
            }
        }
    return out;
}

std::optional<VoxelCoord> GroupField::bounds_max() const {
    std::optional<VoxelCoord> out;
    for (const auto& [key, chunk] : chunks_)
        for (std::size_t i = 0; i < kCells; ++i) {
            if (chunk.data[i] == kNoGroup) continue;
            const VoxelCoord c{
                key.x * kChunkDim + static_cast<std::int32_t>(i % kChunkDim),
                key.y * kChunkDim + static_cast<std::int32_t>((i / kChunkDim) % kChunkDim),
                key.z * kChunkDim +
                    static_cast<std::int32_t>(i / (static_cast<std::size_t>(kChunkDim) * kChunkDim))};
            if (!out) {
                out = c;
            } else {
                out->x = std::max(out->x, c.x);
                out->y = std::max(out->y, c.y);
                out->z = std::max(out->z, c.z);
            }
        }
    return out;
}

}  // namespace voxel
}  // namespace clay
