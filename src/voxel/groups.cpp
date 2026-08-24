#include "clay/voxel/groups.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

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

namespace {

// The 6-neighbourhood. Face-adjacency rather than 26, because a diagonal step
// grows a region by sqrt(3) cells per step along the diagonal and 1 along an
// axis — so "grow by two" would mean two different distances depending on
// which way the surface happened to run, and an artist reads a grow as a
// uniform offset.
constexpr int kFaceOffsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                    {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

}  // namespace

std::vector<VoxelCoord> GroupField::cells_of(GroupId id) const {
    std::vector<VoxelCoord> out;
    if (id == kNoGroup) return out;
    for (const auto& [key, chunk] : chunks_)
        for (std::size_t i = 0; i < kCells; ++i) {
            if (chunk.data[i] != id) continue;
            out.push_back({key.x * kChunkDim + static_cast<std::int32_t>(i % kChunkDim),
                           key.y * kChunkDim +
                               static_cast<std::int32_t>((i / kChunkDim) % kChunkDim),
                           key.z * kChunkDim +
                               static_cast<std::int32_t>(
                                   i / (static_cast<std::size_t>(kChunkDim) * kChunkDim))});
        }
    return out;
}

std::size_t GroupField::grow(GroupId id, int steps) {
    if (id == kNoGroup || steps <= 0) return 0;
    std::size_t claimed = 0;
    for (int s = 0; s < steps; ++s) {
        // Collected before any is written: growing in place would let a cell
        // claimed this step seed the next one, which turns one step into
        // `steps` and makes grow(1) unpredictable.
        std::vector<VoxelCoord> frontier;
        for (VoxelCoord c : cells_of(id))
            for (const auto& d : kFaceOffsets) {
                const VoxelCoord n{c.x + d[0], c.y + d[1], c.z + d[2]};
                if (get(n) == kNoGroup) frontier.push_back(n);
            }
        if (frontier.empty()) break;
        for (VoxelCoord c : frontier) {
            // Re-checked: two cells of the group can name the same neighbour,
            // and only one of them claims it.
            if (get(c) != kNoGroup) continue;
            set(c, id);
            ++claimed;
        }
    }
    return claimed;
}

std::size_t GroupField::shrink(GroupId id, int steps) {
    if (id == kNoGroup || steps <= 0) return 0;
    std::size_t released = 0;
    for (int s = 0; s < steps; ++s) {
        const std::vector<VoxelCoord> rim = border(id);
        if (rim.empty()) break;
        for (VoxelCoord c : rim) {
            set(c, kNoGroup);
            ++released;
        }
    }
    return released;
}

std::vector<VoxelCoord> GroupField::border(GroupId id) const {
    std::vector<VoxelCoord> out;
    if (id == kNoGroup) return out;
    for (VoxelCoord c : cells_of(id))
        for (const auto& d : kFaceOffsets)
            if (get({c.x + d[0], c.y + d[1], c.z + d[2]}) != id) {
                out.push_back(c);
                break;  // one entry per cell, however many neighbours differ
            }
    return out;
}

void GroupField::invert_visibility() {
    touch();
    std::unordered_set<GroupId> now_hidden;
    for (GroupId g : ids())
        if (hidden_.find(g) == hidden_.end()) now_hidden.insert(g);
    hidden_ = std::move(now_hidden);
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

std::size_t GroupField::fill_from_mask(const MaskField& mask, GroupId id, float threshold) {
    if (id == kNoGroup) return 0;
    std::size_t claimed = 0;
    // Driven by the MASK's painted extent rather than by a region the caller
    // supplies: a mask already knows where it is, and asking for bounds a
    // caller must compute is how the two lattices get to disagree.
    const std::optional<VoxelCoord> lo = mask.bounds_min();
    const std::optional<VoxelCoord> hi = mask.bounds_max();
    if (!lo || !hi) return 0;
    const kernel::cfloat3 world_lo = mask.cell_centre(*lo);
    const kernel::cfloat3 world_hi = mask.cell_centre(*hi);
    const VoxelCoord c0 = cell_at(world_lo);
    const VoxelCoord c1 = cell_at(world_hi);
    for (std::int32_t z = c0.z; z <= c1.z; ++z)
        for (std::int32_t y = c0.y; y <= c1.y; ++y)
            for (std::int32_t x = c0.x; x <= c1.x; ++x) {
                const VoxelCoord c{x, y, z};
                // Sampled at THIS field's cell centre, so a coarse group over a
                // fine mask quantises rather than misaligns.
                if (mask.sample(cell_centre(c)) < threshold) continue;
                if (get(c) == id) continue;
                set(c, id);
                ++claimed;
            }
    return claimed;
}

std::vector<std::uint8_t> GroupField::serialize() const {
    std::vector<std::uint8_t> out;
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    };
    std::uint32_t bits;
    std::memcpy(&bits, &cell_size_, 4);
    put32(bits);

    std::map<std::tuple<int, int, int>, const Chunk*> ordered;  // deterministic
    for (const auto& [key, chunk] : chunks_) ordered[{key.x, key.y, key.z}] = &chunk;
    put32(static_cast<std::uint32_t>(ordered.size()));
    for (const auto& [key, chunk] : ordered) {
        put32(static_cast<std::uint32_t>(std::get<0>(key)));
        put32(static_cast<std::uint32_t>(std::get<1>(key)));
        put32(static_cast<std::uint32_t>(std::get<2>(key)));
        // RLE over 16-bit ids, run-length capped at 65535. A group field is
        // overwhelmingly long runs of one id or of kNoGroup, which is what
        // makes this worth doing on a 32^3 chunk of u16.
        std::vector<std::uint8_t> runs;
        std::size_t i = 0;
        while (i < chunk->data.size()) {
            const GroupId v = chunk->data[i];
            std::size_t run = 1;
            while (i + run < chunk->data.size() && chunk->data[i + run] == v && run < 65535) ++run;
            runs.push_back(static_cast<std::uint8_t>(v & 0xff));
            runs.push_back(static_cast<std::uint8_t>(v >> 8));
            runs.push_back(static_cast<std::uint8_t>(run & 0xff));
            runs.push_back(static_cast<std::uint8_t>(run >> 8));
            i += run;
        }
        put32(static_cast<std::uint32_t>(runs.size()));
        out.insert(out.end(), runs.begin(), runs.end());
    }

    // The hidden set, sorted so the blob is deterministic. Written after the
    // cells so a reader that stops early still has a coherent field — it would
    // show everything, which is the safe direction to fail in.
    std::vector<GroupId> hidden(hidden_.begin(), hidden_.end());
    std::sort(hidden.begin(), hidden.end());
    put32(static_cast<std::uint32_t>(hidden.size()));
    for (GroupId g : hidden) {
        out.push_back(static_cast<std::uint8_t>(g & 0xff));
        out.push_back(static_cast<std::uint8_t>(g >> 8));
    }
    return out;
}

std::optional<GroupField> GroupField::deserialize(const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 0;
    auto get32 = [&](std::uint32_t* v) {
        if (pos + 4 > size) return false;
        *v = static_cast<std::uint32_t>(data[pos]) | (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
             (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
             (static_cast<std::uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    };
    std::uint32_t bits = 0;
    if (!get32(&bits)) return std::nullopt;
    float cell_size = 0.0f;
    std::memcpy(&cell_size, &bits, 4);
    if (!(cell_size > 0.0f)) return std::nullopt;

    GroupField field(cell_size);
    std::uint32_t chunk_count = 0;
    if (!get32(&chunk_count)) return std::nullopt;
    for (std::uint32_t i = 0; i < chunk_count; ++i) {
        std::uint32_t kx = 0, ky = 0, kz = 0, run_bytes = 0;
        if (!get32(&kx) || !get32(&ky) || !get32(&kz) || !get32(&run_bytes)) return std::nullopt;
        if (run_bytes % 4 != 0 || pos + run_bytes > size) return std::nullopt;
        Chunk chunk;
        chunk.data.assign(kCells, kNoGroup);
        std::size_t at = 0;
        for (std::uint32_t r = 0; r < run_bytes; r += 4) {
            const GroupId v = static_cast<GroupId>(data[pos + r] |
                                                   (static_cast<GroupId>(data[pos + r + 1]) << 8));
            const std::size_t run = static_cast<std::size_t>(data[pos + r + 2]) |
                                    (static_cast<std::size_t>(data[pos + r + 3]) << 8);
            if (run == 0 || at + run > kCells) return std::nullopt;
            for (std::size_t k = 0; k < run; ++k) chunk.data[at + k] = v;
            if (v != kNoGroup) chunk.assigned += static_cast<int>(run);
            at += run;
        }
        if (at != kCells) return std::nullopt;
        pos += run_bytes;
        // An all-empty chunk is not stored: a missing chunk is how this lattice
        // spells "nothing here", and keeping one would make a round trip differ
        // from the field it came from.
        if (chunk.assigned == 0) continue;
        field.chunks_.emplace(VoxelCoord{static_cast<std::int32_t>(kx), static_cast<std::int32_t>(ky),
                                         static_cast<std::int32_t>(kz)},
                              std::move(chunk));
    }

    std::uint32_t hidden_count = 0;
    if (!get32(&hidden_count)) return std::nullopt;
    if (pos + static_cast<std::size_t>(hidden_count) * 2 > size) return std::nullopt;
    for (std::uint32_t i = 0; i < hidden_count; ++i) {
        field.hidden_.insert(
            static_cast<GroupId>(data[pos] | (static_cast<GroupId>(data[pos + 1]) << 8)));
        pos += 2;
    }
    return field;
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
