#include "clay/voxel/grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>

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

std::uint8_t VoxelGrid::cell_at(std::size_t level, VoxelCoord c) const {
    const ChunkMap& chunks = levels_[level].chunks;
    auto it = chunks.find(chunk_key(c));
    return it == chunks.end() ? 0 : it->second.data[chunk_offset(c)];
}

// Returns whether the cell actually changed, which is what change_count counts.
bool VoxelGrid::write_cell(std::size_t level, VoxelCoord c, std::uint8_t index) {
    ChunkMap& chunks = levels_[level].chunks;
    VoxelCoord key = chunk_key(c);
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
    if (it->second.occupied == 0) chunks.erase(it);
    return changed;
}

std::uint8_t VoxelGrid::get(VoxelCoord c) const { return cell_at(active_, c); }

void VoxelGrid::set(VoxelCoord c, std::uint8_t index) {
    // Every verb funnels its writes through here, so one compare instruments
    // all of them, and propagation below is charged to the edit rather than
    // counted as further edits.
    if (write_cell(active_, c, index)) ++change_count_;
    if (levels_.size() == 1) return;  // the single-level grid: nothing to carry
    // Down first: it restates this cell's own offset against the parent it just
    // recomputed, which is what the walk back up then replays.
    propagate_down(active_, c);
    propagate_up(active_, c);
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
    levels_.push_back(Level{levels_.back().voxel_size * 0.5f, {}, {}});
    std::size_t fine = levels_.size() - 1;
    subdivide_into(fine);
    return fine;
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
                    for (int i = 0; i < kChildren; ++i) write_cell(fine, child_cell(c, i), v);
                }
    }
}

// A cell differs from its parent or it does not; the map holds exactly the ones
// that do, so it stays the size of the detail rather than the size of the level.
void VoxelGrid::record_detail(std::size_t level, VoxelCoord c) {
    if (level == 0) return;
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
    for (int i = 0; i < kChildren; ++i) {
        VoxelCoord child = child_cell(c, i);
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

std::optional<VoxelCoord> VoxelGrid::bounds_min() const {
    std::optional<VoxelCoord> out;
    for (const auto& [key, chunk] : levels_[active_].chunks) {
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    if (!chunk.data[(static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x])
                        continue;
                    VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                 key.z * kChunkDim + z};
                    if (!out)
                        out = c;
                    else
                        *out = {std::min(out->x, c.x), std::min(out->y, c.y),
                                std::min(out->z, c.z)};
                }
    }
    return out;
}

std::optional<VoxelCoord> VoxelGrid::bounds_max() const {
    std::optional<VoxelCoord> out;
    for (const auto& [key, chunk] : levels_[active_].chunks) {
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    if (!chunk.data[(static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x])
                        continue;
                    VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                 key.z * kChunkDim + z};
                    if (!out)
                        out = c;
                    else
                        *out = {std::max(out->x, c.x), std::max(out->y, c.y),
                                std::max(out->z, c.z)};
                }
    }
    return out;
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
constexpr std::uint32_t kLevelTail = 0x564C4343u;  // "CCLV" little-endian

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
    if (levels_.size() == 1) return out;  // byte-for-byte the stream it always was

    // Finer levels ride along as their OFFSETS ONLY — the cells that differ
    // from the level below — because everything else is reproducible by
    // subdividing. A level carrying no detail therefore costs four bytes, which
    // is the answer to whether a stack has to multiply the file size.
    put32(kLevelTail);
    put32(static_cast<std::uint32_t>(levels_.size()));
    put32(static_cast<std::uint32_t>(active_));
    for (std::size_t i = 1; i < levels_.size(); ++i) {
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
    std::uint32_t tag = 0;
    if (pos + 4 > size || !get32(&tag) || tag != kLevelTail) return grid;
    if (!grid.read_level_tail(data, size, &pos, palette_count)) return std::nullopt;
    return grid;
}

bool VoxelGrid::read_level_tail(const std::uint8_t* data, std::size_t size, std::size_t* pos,
                                std::uint32_t palette_count) {
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
        if (add_level() != i) return false;  // the cap refused; the indices would drift
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

mesh::Mesh VoxelGrid::mesh_greedy(std::size_t level) const {
    mesh::Mesh out;
    if (level >= levels_.size()) return out;
    const ChunkMap& chunks = levels_[level].chunks;
    if (chunks.empty()) return out;
    auto value_at = [&](VoxelCoord c) { return cell_at(level, c); };

    // For each of 6 face directions, sweep slices and greedy-merge rectangles
    // of equal palette index whose faces are exposed.
    struct Dir {
        int axis;     // face normal axis 0/1/2
        int sign;     // +1 or -1
    };
    const Dir dirs[6] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, 1}, {2, -1}};

    auto slice_cell = [&](int a, int u, int v, int axis) {
        // map (slice a, u, v) back to xyz for a given axis
        VoxelCoord c;
        if (axis == 0) c = {a, u, v};
        if (axis == 1) c = {u, a, v};
        if (axis == 2) c = {u, v, a};
        return c;
    };

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
    auto build_slice_mask = [&](const Dir& dir, int a, int slab_index, int u0, int v0, int nu,
                                int nv, std::vector<std::uint8_t>& mask) {
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
    struct Slab {
        int u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    };
    auto axis_of = [](VoxelCoord c, int axis) { return axis == 0 ? c.x : (axis == 1 ? c.y : c.z); };
    auto u_of = [](VoxelCoord c, int axis) { return axis == 0 ? c.y : c.x; };
    auto v_of = [](VoxelCoord c, int axis) { return axis == 2 ? c.y : c.z; };

    for (const Dir& dir : dirs) {
        // slab index along the sweep axis -> the (u, v) box its chunks span
        std::map<int, Slab> slabs;
        for (const auto& [key, chunk] : chunks) {
            if (chunk.occupied <= 0) continue;
            const int sa = axis_of(key, dir.axis);
            const int cu = u_of(key, dir.axis) * kChunkDim;
            const int cv = v_of(key, dir.axis) * kChunkDim;
            auto it = slabs.find(sa);
            if (it == slabs.end()) {
                slabs.emplace(sa, Slab{cu, cu + kChunkDim - 1, cv, cv + kChunkDim - 1});
                continue;
            }
            it->second.u0 = std::min(it->second.u0, cu);
            it->second.u1 = std::max(it->second.u1, cu + kChunkDim - 1);
            it->second.v0 = std::min(it->second.v0, cv);
            it->second.v1 = std::max(it->second.v1, cv + kChunkDim - 1);
        }

        std::vector<std::uint8_t> mask;
        for (const auto& [slab_index, slab] : slabs) {
        const int u0 = slab.u0, v0 = slab.v0;
        const int nu = slab.u1 - slab.u0 + 1, nv = slab.v1 - slab.v0 + 1;
        mask.assign(static_cast<std::size_t>(nu) * nv, 0);
        const int a0 = slab_index * kChunkDim;
        const int a1 = a0 + kChunkDim - 1;

        for (int a = a0; a <= a1; ++a) {
            build_slice_mask(dir, a, slab_index, u0, v0, nu, nv, mask);
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
                    int w = 1;
                    while (u + w < nu && mask[static_cast<std::size_t>(v) * nu + u + w] == idx)
                        ++w;
                    int h = 1;
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
                    for (int dv = 0; dv < h; ++dv)
                        for (int du = 0; du < w; ++du)
                            mask[static_cast<std::size_t>(v + dv) * nu + u + du] = 0;
                    u += w;
                }
        }
        }
    }
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

void VoxelGrid::rasterize_tape(const scene::Tape& tape, const math::Aabb& world_region) {
    if (world_region.empty() || world_region.is_infinite()) return;
    const float vs = voxel_size();
    std::int32_t x0 = static_cast<std::int32_t>(std::floor(world_region.min.x / vs));
    std::int32_t y0 = static_cast<std::int32_t>(std::floor(world_region.min.y / vs));
    std::int32_t z0 = static_cast<std::int32_t>(std::floor(world_region.min.z / vs));
    std::int32_t x1 = static_cast<std::int32_t>(std::floor(world_region.max.x / vs));
    std::int32_t y1 = static_cast<std::int32_t>(std::floor(world_region.max.y / vs));
    std::int32_t z1 = static_cast<std::int32_t>(std::floor(world_region.max.z / vs));
    for (std::int32_t z = z0; z <= z1; ++z)
        for (std::int32_t y = y0; y <= y1; ++y)
            for (std::int32_t x = x0; x <= x1; ++x) {
                cfloat3 center = cf3((static_cast<float>(x) + 0.5f) * vs,
                                     (static_cast<float>(y) + 0.5f) * vs,
                                     (static_cast<float>(z) + 0.5f) * vs);
                kernel::CTapeValue v = tape.eval(center);
                if (v.d < 0.0f) set({x, y, z}, palette_add(v.color, 1.0f / 64.0f));
            }
}

}  // namespace voxel
}  // namespace clay
