// Mask field (voxel-engine spec). See include/clay/voxel/mask.h for why the
// lattice is addressed in world units rather than in a layer's voxel cells.

#include "clay/voxel/mask.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace clay {
namespace voxel {

using kernel::cfloat3;

namespace {

std::int32_t fdiv(std::int32_t a, std::int32_t b) {
    std::int32_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
std::int32_t fmod_pos(std::int32_t a, std::int32_t b) {
    std::int32_t r = a % b;
    return r < 0 ? r + b : r;
}

std::uint8_t quantize(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(v * 255.0f));
}

// Footprint arithmetic, shared with the voxel brushes: size n spans exactly n
// cells per axis, running -((n-1)/2) ..= n/2.
struct Extent {
    int lo, hi;
};
Extent extent_of(int n) { return {-((n - 1) / 2), n / 2}; }

bool in_footprint(int x, int y, int z, int n, Extent e, BrushShape shape) {
    if (shape == BrushShape::Cube) return true;
    int mid = e.lo + e.hi;
    int dx = 2 * x - mid, dy = 2 * y - mid, dz = 2 * z - mid;
    return dx * dx + dy * dy + dz * dz <= n * n;
}

float normalized_distance(int x, int y, int z, int n, Extent e) {
    int mid = e.lo + e.hi;
    float dx = (2.0f * x - mid) * 0.5f;
    float dy = (2.0f * y - mid) * 0.5f;
    float dz = (2.0f * z - mid) * 0.5f;
    float radius = n * 0.5f;
    if (radius <= 0.0f) return 0.0f;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / radius;
}

float falloff_weight(BrushFalloff curve, float d) {
    d = std::clamp(d, 0.0f, 1.0f);
    switch (curve) {
        case BrushFalloff::Linear:
            return 1.0f - d;
        case BrushFalloff::Smooth: {
            float t = 1.0f - d;
            return t * t * (3.0f - 2.0f * t);
        }
        case BrushFalloff::Gaussian:
            return std::exp(-4.5f * d * d);
        case BrushFalloff::Constant:
        default:
            return 1.0f;
    }
}

}  // namespace

VoxelCoord MaskField::chunk_key(VoxelCoord c) {
    return {fdiv(c.x, kChunkDim), fdiv(c.y, kChunkDim), fdiv(c.z, kChunkDim)};
}

std::size_t MaskField::chunk_offset(VoxelCoord c) {
    std::int32_t x = fmod_pos(c.x, kChunkDim);
    std::int32_t y = fmod_pos(c.y, kChunkDim);
    std::int32_t z = fmod_pos(c.z, kChunkDim);
    return (static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x;
}

VoxelCoord MaskField::cell_at(cfloat3 p) const {
    return {static_cast<std::int32_t>(std::floor(p.x / cell_size_)),
            static_cast<std::int32_t>(std::floor(p.y / cell_size_)),
            static_cast<std::int32_t>(std::floor(p.z / cell_size_))};
}

cfloat3 MaskField::cell_centre(VoxelCoord c) const {
    return kernel::cf3((c.x + 0.5f) * cell_size_, (c.y + 0.5f) * cell_size_,
                       (c.z + 0.5f) * cell_size_);
}

// -- values ------------------------------------------------------------------

float MaskField::get(VoxelCoord c) const {
    auto it = chunks_.find(chunk_key(c));
    if (it == chunks_.end()) return 0.0f;
    return it->second.data[chunk_offset(c)] / 255.0f;
}

void MaskField::set(VoxelCoord c, float value) {
    std::uint8_t q = quantize(value);
    VoxelCoord key = chunk_key(c);
    auto it = chunks_.find(key);
    if (it == chunks_.end()) {
        if (q == 0) return;  // storing zero would only allocate an empty chunk
        it = chunks_.emplace(key, Chunk{}).first;
        it->second.data.assign(static_cast<std::size_t>(kChunkDim) * kChunkDim * kChunkDim, 0);
    }
    std::uint8_t& cell = it->second.data[chunk_offset(c)];
    if (cell == 0 && q != 0) ++it->second.painted;
    if (cell != 0 && q == 0) --it->second.painted;
    cell = q;
    if (it->second.painted == 0) chunks_.erase(it);
}

// -- painting ----------------------------------------------------------------

void MaskField::paint(cfloat3 world_centre, const BrushParams& p, float target) {
    paint(cell_at(world_centre), p, target);
}

void MaskField::paint(VoxelCoord c, const BrushParams& p, float target) {
    Extent e = extent_of(p.size);
    for (int z = e.lo; z <= e.hi; ++z)
        for (int y = e.lo; y <= e.hi; ++y)
            for (int x = e.lo; x <= e.hi; ++x) {
                if (!in_footprint(x, y, z, p.size, e, p.shape)) continue;
                float w = falloff_weight(p.falloff, normalized_distance(x, y, z, p.size, e)) *
                          std::clamp(p.strength, 0.0f, 1.0f);
                if (w <= 0.0f) continue;
                VoxelCoord cell{c.x + x, c.y + y, c.z + z};
                float v = get(cell);
                set(cell, v + (std::clamp(target, 0.0f, 1.0f) - v) * w);
            }
}

// -- region operations -------------------------------------------------------

void MaskField::clear() { chunks_.clear(); }

void MaskField::invert() {
    // Defined over the painted region: a sparse field has no finite complement
    // to invert into, so inverting is "flip what has been touched".
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        Chunk& ch = it->second;
        ch.painted = 0;
        for (std::uint8_t& v : ch.data) {
            v = static_cast<std::uint8_t>(255 - v);
            if (v != 0) ++ch.painted;
        }
        it = ch.painted == 0 ? chunks_.erase(it) : std::next(it);
    }
}

// -- bounded region operations -----------------------------------------------

namespace {

// The cells whose centres can lie in `region`, or nothing when there is no such
// finite set. Shared by fill and invert_within so the two cannot disagree about
// where a region's edge is.
struct CellRange {
    VoxelCoord lo, hi;
};

std::optional<CellRange> cells_of(const MaskField& m, const math::Aabb& region) {
    if (!m.region_is_walkable(region)) return std::nullopt;
    return CellRange{m.cell_at(region.min), m.cell_at(region.max)};
}

bool centre_in(const math::Aabb& region, cfloat3 p) {
    return p.x >= region.min.x && p.x <= region.max.x && p.y >= region.min.y &&
           p.y <= region.max.y && p.z >= region.min.z && p.z <= region.max.z;
}

}  // namespace

bool MaskField::region_is_walkable(const math::Aabb& region) const {
    if (region.empty()) return false;
    // In double, and per axis before the product: a box near FLT_MAX overflows
    // the lattice's own int32 cell indices long before its volume overflows a
    // float, and `is_infinite` only catches the exact sentinel. Counting first
    // is what turns "an arithmetic mistake upstream" into a refusal rather than
    // into a loop over garbage bounds.
    // Named per axis rather than indexed through &min.x: walking off the end of
    // one member into the next is undefined however reliably it works, and the
    // three cases are three lines.
    const double scale = 1.0 / static_cast<double>(cell_size_);
    double cells = 1.0;
    const auto axis = [&cells, scale](float low, float high) {
        const double lo = low * scale, hi = high * scale;
        if (!(lo > -2147483648.0 && hi < 2147483647.0)) return false;
        const double span = std::floor(hi) - std::floor(lo) + 1.0;
        if (span > kMaxRegionCells) return false;
        cells *= span;
        return cells <= kMaxRegionCells;
    };
    return axis(region.min.x, region.max.x) && axis(region.min.y, region.max.y) &&
           axis(region.min.z, region.max.z);
}

void MaskField::fill(const math::Aabb& region, float value) {
    std::optional<CellRange> r = cells_of(*this, region);
    if (!r) return;
    for (std::int32_t z = r->lo.z; z <= r->hi.z; ++z)
        for (std::int32_t y = r->lo.y; y <= r->hi.y; ++y)
            for (std::int32_t x = r->lo.x; x <= r->hi.x; ++x) {
                VoxelCoord c{x, y, z};
                if (centre_in(region, cell_centre(c))) set(c, value);
            }
}

void MaskField::invert_within(const math::Aabb& region) {
    std::optional<CellRange> r = cells_of(*this, region);
    if (!r) return;
    for (std::int32_t z = r->lo.z; z <= r->hi.z; ++z)
        for (std::int32_t y = r->lo.y; y <= r->hi.y; ++y)
            for (std::int32_t x = r->lo.x; x <= r->hi.x; ++x) {
                VoxelCoord c{x, y, z};
                // Unpainted cells are inverted too — that is the entire point of
                // the bounded form, and what invert() structurally cannot do.
                if (centre_in(region, cell_centre(c))) set(c, 1.0f - get(c));
            }
}

// Dense scratch over the painted region padded by `pad`, so a dilation or a
// blur can grow past the currently allocated chunks. `reduce` sees the padded
// buffer and returns the new value for one cell.
template <typename Fn>
void MaskField::neighbourhood_op(int pad, Fn&& reduce) {
    std::optional<VoxelCoord> lo = bounds_min(), hi = bounds_max();
    if (!lo || !hi) return;
    // The padded bounding box still decides WHERE the operation applies and
    // where a neighbour is clamped to the cell itself. What it no longer does
    // is get allocated: a mask is sparse and its bounding box is not, so two
    // painted specks far apart used to size two dense buffers spanning
    // everything between them — seconds and gigabytes for a few hundred cells,
    // and an int overflow in `b.x - a.x + 1` before that for coordinates a
    // deserialized mask may legitimately carry.
    const VoxelCoord a{lo->x - pad, lo->y - pad, lo->z - pad};
    const VoxelCoord b{hi->x + pad, hi->y + pad, hi->z + pad};

    // Cells worth visiting: every occupied chunk grown by the pad. Anything
    // further out reads as empty and reduces to itself, which set() drops.
    // Chunk keys are snapshotted because staging is applied afterwards and
    // set() may create chunks.
    std::vector<VoxelCoord> keys;
    keys.reserve(chunks_.size());
    for (const auto& [key, chunk] : chunks_) keys.push_back(key);

    // One dense window per occupied chunk rather than one over everything: the
    // window is read with a halo of pad+1 so a staged cell's own neighbours are
    // always inside it, and the result is staged whole and applied afterwards.
    // Applying as we go would let one chunk's pad ring read another's output,
    // where the dense sweep took a single snapshot before writing any of it.
    // Each chunk's block is CLIPPED to the padded box. That keeps the compact
    // case as cheap as the dense sweep was — a brush stamp's box is smaller
    // than one chunk, and processing a whole chunk for it would be slower, not
    // faster — while a far-apart pair costs one bounded block per chunk instead
    // of everything between them.
    struct Block {
        VoxelCoord lo;                   // world coords of the block's origin
        int nx = 0, ny = 0, nz = 0;
        std::vector<std::uint8_t> dst;
    };
    std::vector<Block> staged;
    staged.reserve(keys.size());
    std::vector<std::uint8_t> src;

    for (const VoxelCoord& key : keys) {
        Block blk;
        blk.lo = {std::max(key.x * kChunkDim - pad, a.x), std::max(key.y * kChunkDim - pad, a.y),
                  std::max(key.z * kChunkDim - pad, a.z)};
        const VoxelCoord blk_hi{std::min(key.x * kChunkDim + kChunkDim - 1 + pad, b.x),
                            std::min(key.y * kChunkDim + kChunkDim - 1 + pad, b.y),
                            std::min(key.z * kChunkDim + kChunkDim - 1 + pad, b.z)};
        blk.nx = blk_hi.x - blk.lo.x + 1;
        blk.ny = blk_hi.y - blk.lo.y + 1;
        blk.nz = blk_hi.z - blk.lo.z + 1;
        if (blk.nx <= 0 || blk.ny <= 0 || blk.nz <= 0) continue;

        // Read with a one-cell halo so a block cell's own neighbours are always
        // in the buffer; the halo is read, never written.
        const int wx = blk.nx + 2, wy = blk.ny + 2, wz = blk.nz + 2;
        src.assign(static_cast<std::size_t>(wx) * wy * wz, 0);
        auto wat = [&](int x, int y, int z) {
            return (static_cast<std::size_t>(z) * wy + y) * wx + x;
        };
        for (int z = 0; z < wz; ++z)
            for (int y = 0; y < wy; ++y)
                for (int x = 0; x < wx; ++x)
                    src[wat(x, y, z)] =
                        quantize(get({blk.lo.x + x - 1, blk.lo.y + y - 1, blk.lo.z + z - 1}));

        blk.dst.assign(static_cast<std::size_t>(blk.nx) * blk.ny * blk.nz, 0);
        for (int z = 0; z < blk.nz; ++z)
            for (int y = 0; y < blk.ny; ++y)
                for (int x = 0; x < blk.nx; ++x) {
                    const std::int32_t gx = blk.lo.x + x, gy = blk.lo.y + y, gz = blk.lo.z + z;
                    // 6-neighbourhood plus self, clamped at the PADDED BOUNDING
                    // BOX exactly as the dense sweep clamped at its buffer edge
                    // — this block's own edge is not a boundary of anything.
                    const std::uint8_t self = src[wat(x + 1, y + 1, z + 1)];
                    const std::uint8_t n[6] = {
                        gx > a.x ? src[wat(x, y + 1, z + 1)] : self,
                        gx < b.x ? src[wat(x + 2, y + 1, z + 1)] : self,
                        gy > a.y ? src[wat(x + 1, y, z + 1)] : self,
                        gy < b.y ? src[wat(x + 1, y + 2, z + 1)] : self,
                        gz > a.z ? src[wat(x + 1, y + 1, z)] : self,
                        gz < b.z ? src[wat(x + 1, y + 1, z + 2)] : self,
                    };
                    blk.dst[(static_cast<std::size_t>(z) * blk.ny + y) * blk.nx + x] =
                        reduce(self, n);
                }
        staged.push_back(std::move(blk));
    }

    // Applied only now: a cell in two blocks' overlap gets the same answer from
    // both, but neither may read the other's output.
    for (const Block& blk : staged)
        for (int z = 0; z < blk.nz; ++z)
            for (int y = 0; y < blk.ny; ++y)
                for (int x = 0; x < blk.nx; ++x)
                    set({blk.lo.x + x, blk.lo.y + y, blk.lo.z + z},
                        blk.dst[(static_cast<std::size_t>(z) * blk.ny + y) * blk.nx + x] / 255.0f);
}

void MaskField::expand(int steps) {
    for (int i = 0; i < steps; ++i)
        neighbourhood_op(1, [](std::uint8_t self, const std::uint8_t (&n)[6]) {
            std::uint8_t m = self;
            for (std::uint8_t v : n) m = std::max(m, v);
            return m;
        });
}

void MaskField::contract(int steps) {
    // Padded like expand: a cell on the painted region's face has an unpainted
    // neighbour just outside it, and erosion has to see that zero.
    for (int i = 0; i < steps; ++i)
        neighbourhood_op(1, [](std::uint8_t self, const std::uint8_t (&n)[6]) {
            std::uint8_t m = self;
            for (std::uint8_t v : n) m = std::min(m, v);
            return m;
        });
}

void MaskField::smooth(int iterations) {
    for (int i = 0; i < iterations; ++i)
        neighbourhood_op(1, [](std::uint8_t self, const std::uint8_t (&n)[6]) {
            int sum = self;
            for (std::uint8_t v : n) sum += v;
            return static_cast<std::uint8_t>(sum / 7);
        });
}

// -- queries -----------------------------------------------------------------

std::size_t MaskField::painted_count() const {
    std::size_t total = 0;
    for (const auto& [key, chunk] : chunks_) total += static_cast<std::size_t>(chunk.painted);
    return total;
}

std::optional<VoxelCoord> MaskField::bounds_min() const {
    std::optional<VoxelCoord> best;
    for (const auto& [key, chunk] : chunks_)
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    if (chunk.data[(static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x] ==
                        0)
                        continue;
                    VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                 key.z * kChunkDim + z};
                    if (!best) {
                        best = c;
                    } else {
                        best->x = std::min(best->x, c.x);
                        best->y = std::min(best->y, c.y);
                        best->z = std::min(best->z, c.z);
                    }
                }
    return best;
}

std::optional<VoxelCoord> MaskField::bounds_max() const {
    std::optional<VoxelCoord> best;
    for (const auto& [key, chunk] : chunks_)
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    if (chunk.data[(static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x] ==
                        0)
                        continue;
                    VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                 key.z * kChunkDim + z};
                    if (!best) {
                        best = c;
                    } else {
                        best->x = std::max(best->x, c.x);
                        best->y = std::max(best->y, c.y);
                        best->z = std::max(best->z, c.z);
                    }
                }
    return best;
}

// -- serialization -----------------------------------------------------------

std::vector<std::uint8_t> MaskField::serialize() const {
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
        std::vector<std::uint8_t> runs;
        std::size_t i = 0;
        while (i < chunk->data.size()) {
            std::uint8_t v = chunk->data[i];
            std::size_t run = 1;
            while (i + run < chunk->data.size() && chunk->data[i + run] == v && run < 255) ++run;
            runs.push_back(v);
            runs.push_back(static_cast<std::uint8_t>(run));
            i += run;
        }
        put32(static_cast<std::uint32_t>(runs.size()));
        out.insert(out.end(), runs.begin(), runs.end());
    }
    return out;
}

std::optional<MaskField> MaskField::deserialize(const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 0;
    auto get32 = [&](std::uint32_t* v) {
        if (pos + 4 > size) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) *v |= static_cast<std::uint32_t>(data[pos++]) << (i * 8);
        return true;
    };
    std::uint32_t bits, chunk_count;
    if (!get32(&bits) || !get32(&chunk_count)) return std::nullopt;
    float cell_size;
    std::memcpy(&cell_size, &bits, 4);
    // `!(cell_size > 0)` rejects NaN and zero; infinity needs saying too, since
    // a world coordinate divided by it floors to a cell of zero everywhere.
    if (!(cell_size > 0.0f) || !std::isfinite(cell_size)) return std::nullopt;

    MaskField m(cell_size);
    // Same decode-amplification bound as VoxelGrid: 16 header bytes plus a
    // 2-byte run per chunk, against 32 KiB of decoded cells.
    if (chunk_count > (size - pos) / 18) return std::nullopt;
    const std::size_t chunk_cells = static_cast<std::size_t>(kChunkDim) * kChunkDim * kChunkDim;
    for (std::uint32_t n = 0; n < chunk_count; ++n) {
        std::uint32_t kx, ky, kz, run_bytes;
        if (!get32(&kx) || !get32(&ky) || !get32(&kz) || !get32(&run_bytes)) return std::nullopt;
        if (run_bytes % 2 != 0 || pos + run_bytes > size) return std::nullopt;
        Chunk chunk;
        chunk.data.reserve(chunk_cells);
        for (std::uint32_t i = 0; i < run_bytes; i += 2) {
            std::uint8_t v = data[pos + i];
            std::uint8_t run = data[pos + i + 1];
            if (run == 0 || chunk.data.size() + run > chunk_cells) return std::nullopt;
            chunk.data.insert(chunk.data.end(), run, v);
            if (v != 0) chunk.painted += run;
        }
        pos += run_bytes;
        if (chunk.data.size() != chunk_cells) return std::nullopt;
        if (chunk.painted > 0)
            m.chunks_.emplace(VoxelCoord{static_cast<std::int32_t>(kx),
                                         static_cast<std::int32_t>(ky),
                                         static_cast<std::int32_t>(kz)},
                              std::move(chunk));
    }
    return m;
}

}  // namespace voxel
}  // namespace clay
