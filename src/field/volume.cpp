// Sampled fields (sdf-kernels spec, add-sampled-fields). See
// include/clay/field/volume.h for why a narrow band rides in the tape's blob
// rather than behind a resource handle.

#include "clay/field/volume.h"

#include "clay/bytes.h"

#include "clay/parallel/thread_pool.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clay {
namespace field {

std::size_t FieldVolume::bytes() const {
    // data_ is the term that matters: kBrickSamples floats per stored brick,
    // and a volume sampled at a fine cell size over a real region runs to
    // megabytes. index_ and far_ are per brick whether stored or not, so they
    // are the floor a sparse volume pays.
    return sizeof(FieldVolume) + vector_bytes(index_) + vector_bytes(far_) +
           vector_bytes(data_) + vector_bytes(colors_);
}

using kernel::cf3;
using kernel::cfloat3;

namespace {

int floor_div(int a, int b) {
    int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::size_t sample_index(int x, int y, int z) {
    const int n = kBrickDim + 1;
    return static_cast<std::size_t>((z * n + y) * n + x);
}

// What the samples of one brick say about it.
struct BrickScan {
    bool near_surface;  // something landed within the band: the brick is kept
    bool any_inside;    // ...and if not, which side the whole of it is on
};

BrickScan scan_block(const float* block, float band) {
    BrickScan scan{false, false};
    for (int i = 0; i < kBrickSamples; ++i) {
        if (std::abs(block[i]) <= band) scan.near_surface = true;
        if (block[i] < 0.0f) scan.any_inside = true;
    }
    return scan;
}

// The causal half of a 3x3x3 neighbourhood: every offset that a forward sweep
// has already visited. A backward sweep uses the same table negated. Chebyshev
// distance, so each of them is one step whether it is a face, an edge or a
// corner.
constexpr int kHalfNeighbourhood[13][3] = {
    {-1, -1, -1}, {0, -1, -1}, {1, -1, -1}, {-1, 0, -1}, {0, 0, -1},
    {1, 0, -1},   {-1, 1, -1}, {0, 1, -1},  {1, 1, -1},  {-1, -1, 0},
    {0, -1, 0},   {1, -1, 0},  {-1, 0, 0},
};

// One chamfer sweep over the brick grid. `dir` is +1 forward and -1 backward;
// the two together give an exact Chebyshev transform.
void chamfer_pass(std::vector<std::int32_t>& steps, const std::int32_t count[3], int dir) {
    const int nx = count[0], ny = count[1], nz = count[2];
    const int total = nx * ny * nz;
    for (int k = 0; k < total; ++k) {
        const int i = dir > 0 ? k : total - 1 - k;
        const int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
        for (const auto& d : kHalfNeighbourhood) {
            const int sx = x + dir * d[0], sy = y + dir * d[1], sz = z + dir * d[2];
            if (sx < 0 || sy < 0 || sz < 0 || sx >= nx || sy >= ny || sz >= nz) continue;
            std::int32_t candidate = steps[static_cast<std::size_t>((sz * ny + sy) * nx + sx)] + 1;
            if (candidate < steps[static_cast<std::size_t>(i)])
                steps[static_cast<std::size_t>(i)] = candidate;
        }
    }
}

}  // namespace

cfloat3 FieldVolume::BrickGrid::sample_position(std::size_t slot, int i) const {
    const int n = kBrickDim + 1;
    const int bx = static_cast<int>(slot) % bcount[0];
    const int by = (static_cast<int>(slot) / bcount[0]) % bcount[1];
    const int bz = static_cast<int>(slot) / (bcount[0] * bcount[1]);
    const int x = i % n, y = (i / n) % n, z = i / (n * n);
    return origin + cf3(static_cast<float>(bx * kBrickDim + x),
                        static_cast<float>(by * kBrickDim + y),
                        static_cast<float>(bz * kBrickDim + z)) *
                        cell_size;
}

math::Aabb FieldVolume::BrickGrid::brick_box(std::size_t slot) const {
    math::Aabb box;
    box.expand(sample_position(slot, 0));
    box.expand(sample_position(slot, kBrickSamples - 1));
    return box;
}

FieldVolume FieldVolume::sample(const std::function<float(cfloat3)>& f, const math::Aabb& region,
                                float cell_size, float band, parallel::CancelToken* token,
                                bool* out_cancelled) {
    // The serial fill: every sample through `f`, at exactly the positions the
    // grid describes. The three axes are walked as ONE index rather than three
    // nested loops — sample_index(x, y, z) is exactly that linear index, so
    // nesting bought nothing except depth.
    return sample_blocks(
        [&f](const BrickGrid& grid, std::size_t first, std::size_t count, float* out) {
            for (std::size_t s = 0; s < count; ++s)
                for (int i = 0; i < kBrickSamples; ++i)
                    out[s * kBrickSamples + i] = f(grid.sample_position(first + s, i));
        },
        region, cell_size, band, token, out_cancelled);
}

FieldVolume FieldVolume::sample_parallel(const std::function<float(cfloat3)>& f,
                                         const math::Aabb& region, float cell_size, float band) {
    // The same fill, over the same windows, split across the pool. The window
    // sample_blocks already uses was sized "enough bricks per call to occupy a
    // thread pool" — this is that call arriving.
    //
    // Disjoint output slices and a value that depends only on its own position,
    // so the result cannot depend on how the work was split.
    return sample_blocks(
        [&f](const BrickGrid& grid, std::size_t first, std::size_t count, float* out) {
            parallel::for_range(count, 1, [&](std::size_t b, std::size_t e) {
                for (std::size_t s = b; s < e; ++s)
                    for (int i = 0; i < kBrickSamples; ++i)
                        out[s * kBrickSamples + i] = f(grid.sample_position(first + s, i));
            });
        },
        region, cell_size, band);
}

namespace {

// 0x00RRGGBB. Colour reaches a volume from a palette or from a float colour
// field, and 8 bits a channel is finer than either resolves; three floats would
// cost three words per sample where this costs one.
std::uint32_t pack_color(cfloat3 c) {
    auto q = [](float v) {
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
    };
    return (q(c.x) << 16) | (q(c.y) << 8) | q(c.z);
}

cfloat3 unpack_color(std::uint32_t p) {
    constexpr float k = 1.0f / 255.0f;
    return kernel::cf3(static_cast<float>((p >> 16) & 0xFFu) * k,
                       static_cast<float>((p >> 8) & 0xFFu) * k,
                       static_cast<float>(p & 0xFFu) * k);
}

}  // namespace

FieldVolume FieldVolume::sample_colored(const std::function<float(cfloat3)>& f,
                                        const std::function<cfloat3(cfloat3)>& c,
                                        const math::Aabb& region, float cell_size, float band) {
    // The distance first, through the ordinary path — so the sparsity, the far
    // bounds and the measured Lipschitz are decided exactly as they are for an
    // uncoloured volume, and the batched-fill contract an external evaluator
    // relies on is untouched.
    FieldVolume v = sample(f, region, cell_size, band);
    if (!c || v.data_.empty()) return v;

    v.fill_colors(c);
    return v;
}

void FieldVolume::fill_colors_blocks(const ColorBlockFill& fill) {
    // Same samples as fill_colors, handed over in windows. The window is sized
    // to occupy a pool without holding every sample's colour at once, which is
    // the same trade sample_blocks makes for the distances.
    if (!fill || data_.empty()) return;
    const BrickGrid grid{origin_, cell_size_, band_, {bcount_[0], bcount_[1], bcount_[2]}};
    colors_.assign(data_.size(), 0u);

    constexpr std::size_t kWindowBricks = 512;
    std::vector<float> points(kWindowBricks * kBrickSamples * 3);
    std::vector<float> rgb(kWindowBricks * kBrickSamples * 3);
    std::vector<std::int32_t> entries;
    entries.reserve(kWindowBricks);

    for (std::size_t slot = 0; slot < index_.size();) {
        entries.clear();
        std::size_t n = 0;
        // Gather the next window of STORED bricks; the empty ones have no
        // samples to colour.
        for (; slot < index_.size() && entries.size() < kWindowBricks; ++slot) {
            const std::int32_t entry = index_[slot];
            if (entry == kBrickEmpty) continue;
            for (int i = 0; i < kBrickSamples; ++i) {
                const cfloat3 p = grid.sample_position(slot, i);
                points[n * 3] = p.x;
                points[n * 3 + 1] = p.y;
                points[n * 3 + 2] = p.z;
                ++n;
            }
            entries.push_back(entry);
        }
        if (entries.empty()) break;
        fill(points.data(), n, rgb.data());
        std::size_t at = 0;
        for (const std::int32_t entry : entries)
            for (int i = 0; i < kBrickSamples; ++i, ++at)
                colors_[static_cast<std::size_t>(entry) + static_cast<std::size_t>(i)] =
                    pack_color(cf3(rgb[at * 3], rgb[at * 3 + 1], rgb[at * 3 + 2]));
    }
}

void FieldVolume::fill_colors(const std::function<cfloat3(cfloat3)>& c) {
    // Only the samples that were KEPT. The bricks sparsity dropped are empty
    // space; a colour there would be a colour for nothing, and asking for one
    // would cost as much as the distance did.
    if (!c || data_.empty()) return;
    const BrickGrid grid{origin_, cell_size_, band_, {bcount_[0], bcount_[1], bcount_[2]}};
    colors_.assign(data_.size(), 0u);
    for (std::size_t slot = 0; slot < index_.size(); ++slot) {
        const std::int32_t entry = index_[slot];
        if (entry == kBrickEmpty) continue;
        for (int i = 0; i < kBrickSamples; ++i)
            colors_[static_cast<std::size_t>(entry) + static_cast<std::size_t>(i)] =
                pack_color(c(grid.sample_position(slot, i)));
    }
}

cfloat3 FieldVolume::eval_color(cfloat3 p) const {
    // The same eight samples the distance interpolates, by the same rule. A
    // nearest-sample read would facet a surface that has none.
    if (colors_.empty()) return kernel::cf3(0.7f, 0.7f, 0.7f);
    cfloat3 g = (p - origin_) * (1.0f / cell_size_);
    int cx = static_cast<int>(std::floor(g.x));
    int cy = static_cast<int>(std::floor(g.y));
    int cz = static_cast<int>(std::floor(g.z));
    cx = std::clamp(cx, 0, bcount_[0] * kBrickDim - 1);
    cy = std::clamp(cy, 0, bcount_[1] * kBrickDim - 1);
    cz = std::clamp(cz, 0, bcount_[2] * kBrickDim - 1);

    const int bx = cx / kBrickDim, by = cy / kBrickDim, bz = cz / kBrickDim;
    const std::size_t slot = static_cast<std::size_t>((bz * bcount_[1] + by) * bcount_[0] + bx);
    const std::int32_t entry = index_[slot];
    // No samples here: empty space carries no colour of its own, and the
    // caller falls back to the item's, which is what the tape does.
    if (entry == kBrickEmpty) return kernel::cf3(0.7f, 0.7f, 0.7f);

    const std::uint32_t* block = colors_.data() + entry;
    const int lx = cx - bx * kBrickDim, ly = cy - by * kBrickDim, lz = cz - bz * kBrickDim;
    const float fx = std::clamp(g.x - static_cast<float>(cx), 0.0f, 1.0f);
    const float fy = std::clamp(g.y - static_cast<float>(cy), 0.0f, 1.0f);
    const float fz = std::clamp(g.z - static_cast<float>(cz), 0.0f, 1.0f);

    auto at = [&](int x, int y, int z) {
        return unpack_color(block[sample_index(lx + x, ly + y, lz + z)]);
    };
    auto mix = [](cfloat3 a, cfloat3 b, float t) { return a * (1.0f - t) + b * t; };
    const cfloat3 c00 = mix(at(0, 0, 0), at(1, 0, 0), fx);
    const cfloat3 c10 = mix(at(0, 1, 0), at(1, 1, 0), fx);
    const cfloat3 c01 = mix(at(0, 0, 1), at(1, 0, 1), fx);
    const cfloat3 c11 = mix(at(0, 1, 1), at(1, 1, 1), fx);
    return mix(mix(c00, c10, fy), mix(c01, c11, fy), fz);
}

FieldVolume FieldVolume::sample_blocks(const BrickBlockFill& fill, const math::Aabb& region,
                                       float cell_size, float band,
                                       parallel::CancelToken* token, bool* out_cancelled) {
                                       if (out_cancelled) *out_cancelled = false;
    FieldVolume v;
    v.cell_size_ = cell_size > 0.0f ? cell_size : 0.05f;
    v.band_ = band > 0.0f ? band : v.cell_size_ * 3.0f;
    // A band thinner than the sample spacing cannot describe anything the
    // samples do not already say, and it would drive far_value() to zero,
    // which stalls a marcher rather than stepping it.
    v.band_ = std::max(v.band_, v.cell_size_ * 2.0f);
    if (region.empty()) return v;

    v.origin_ = region.min;
    // Cells spanning the region, rounded out to whole bricks.
    auto cells_for = [&](float extent) {
        return std::max(1, static_cast<int>(std::ceil(extent / v.cell_size_)));
    };
    cfloat3 extent = region.max - region.min;
    int cells[3] = {cells_for(extent.x), cells_for(extent.y), cells_for(extent.z)};
    for (int a = 0; a < 3; ++a) v.bcount_[a] = floor_div(cells[a] - 1, kBrickDim) + 1;

    const std::size_t total =
        static_cast<std::size_t>(v.bcount_[0]) * v.bcount_[1] * v.bcount_[2];
    v.index_.assign(total, kBrickEmpty);
    v.far_.assign(total, 1.0f);  // sign only for now; the magnitude comes after

    // Brick by brick rather than through one dense grid: the halo means 42%
    // of samples are taken twice, which is a better trade than holding a dense
    // volume in memory for a region that is mostly empty by construction. The
    // window keeps that property for a batched fill — enough bricks per call
    // to occupy a thread pool, never the whole region's samples at once.
    constexpr std::size_t kWindowBricks = 512;
    const BrickGrid grid{v.origin_, v.cell_size_, v.band_,
                         {v.bcount_[0], v.bcount_[1], v.bcount_[2]}};
    std::vector<float> blocks(std::min(total, kWindowBricks) * kBrickSamples);
    for (std::size_t first = 0; first < total; first += kWindowBricks) {
        // The checkpoint is the window boundary the loop already has, so the
        // cost is one relaxed load per 512 bricks rather than one per sample.
        if (parallel::cancelled(token)) {
            if (out_cancelled) *out_cancelled = true;
            return v;  // discarded by the caller; see the header
        }
        if (token) {
            token->advance(first, total ? static_cast<float>(first) / static_cast<float>(total)
                                        : 1.0f);
        }
        const std::size_t count = std::min(kWindowBricks, total - first);
        fill(grid, first, count, blocks.data());
        for (std::size_t s = 0; s < count; ++s) {
            const float* block = blocks.data() + s * kBrickSamples;
            const BrickScan scan = scan_block(block, v.band_);
            if (scan.near_surface) {
                v.index_[first + s] = static_cast<std::int32_t>(v.data_.size());
                v.data_.insert(v.data_.end(), block, block + kBrickSamples);
            } else {
                // No samples: the whole brick is on one side, and which side
                // is the thing a band alone could not tell us. The magnitude
                // comes from build_far_bounds; this only records the sign.
                v.far_[first + s] = scan.any_inside ? -1.0f : 1.0f;
            }
        }
    }
    v.build_far_bounds();
    // Measured, not assumed to be 1. This was the one volume producer that did
    // not measure — flatten, the topological move and the mask extrude all do —
    // and the omission was a real overclaim rather than a tidiness point: baking
    // a chain of two polish passes stored samples varying at fourteen times the
    // cell size and declared them 1-Lipschitz, licensing exactly the overstep
    // the declared bound exists to prevent. A field that IS 1-Lipschitz still
    // measures 1, so nothing that was honest before pays for this.
    v.sample_lipschitz_ = v.measure_sample_lipschitz();
    // Hand back what the growth overshot. `data_` is built by repeated insert
    // with no reserve, because the count is not known until every brick has
    // been scanned, so vector doubling leaves it holding as much as twice what
    // it needs. A baked volume is immutable and long-lived — it becomes the
    // layer's one item and stays for the session — so that overshoot is not
    // headroom for later growth, it is a permanent cost.
    //
    // It measured: a 0.015-cell bake with a 0.12 band cost +75 MB against
    // sdf_consolidate's +6, and on a tablet that peak is what the next
    // allocation has to fit above, because the allocator keeps a process's
    // high-water mark rather than returning it. BrickCache::trim_to already
    // shrinks its own buffers for the same reason.
    v.data_.shrink_to_fit();
    v.colors_.shrink_to_fit();
    v.index_.shrink_to_fit();
    v.far_.shrink_to_fit();
    return v;
}

// How far an empty brick may claim to be from the surface. The surface only
// ever lies inside a brick that HAS samples, so the gap to the nearest such
// brick is a genuine lower bound — and unlike a flat band width it grows, which
// is what lets a marcher cross the empty majority of the region in a sane
// number of steps instead of creeping.
void FieldVolume::build_far_bounds() {
    const std::size_t total = far_.size();
    // Chebyshev distance in bricks to the nearest brick that has samples, by
    // the usual two-sweep chamfer.
    const std::int32_t unreached = static_cast<std::int32_t>(total) + 1;
    std::vector<std::int32_t> steps(total, unreached);
    for (std::size_t i = 0; i < total; ++i)
        if (index_[i] >= 0) steps[i] = 0;
    chamfer_pass(steps, bcount_, +1);
    chamfer_pass(steps, bcount_, -1);

    const float brick = static_cast<float>(kBrickDim) * cell_size_;
    const float floor_value = far_value();
    // Nothing has samples at all: the surface is not in this box, so the box
    // itself is the only thing that can be said, and eval() says it.
    const float ceiling = brick * static_cast<float>(bcount_[0] + bcount_[1] + bcount_[2]);
    for (std::size_t i = 0; i < total; ++i) {
        if (index_[i] >= 0) {
            far_[i] = 0.0f;
            continue;
        }
        // Less one brick: that is the gap between this brick's box and the
        // nearest stored one, which is the most any point in here can claim.
        const float bound = steps[i] < unreached
                                ? std::max(static_cast<float>(steps[i] - 1) * brick, floor_value)
                                : ceiling;
        far_[i] = far_[i] < 0.0f ? -bound : bound;
    }
}

math::Aabb FieldVolume::bounds() const {
    if (empty()) return math::Aabb();
    cfloat3 span = cf3(static_cast<float>(bcount_[0] * kBrickDim),
                       static_cast<float>(bcount_[1] * kBrickDim),
                       static_cast<float>(bcount_[2] * kBrickDim)) *
                   cell_size_;
    math::Aabb b;
    b.expand(origin_);
    b.expand(origin_ + span);
    return b;
}

// Half a cell diagonal, the furthest a point inside a brick can be from the
// nearest sample that was tested against the band.
float FieldVolume::far_value() const {
    return std::max(band_ - 0.87f * cell_size_, 0.0f);
}

bool FieldVolume::has_samples_at(cfloat3 p) const {
    if (empty()) return false;
    math::Aabb box = bounds();
    if (p.x < box.min.x || p.y < box.min.y || p.z < box.min.z) return false;
    if (p.x > box.max.x || p.y > box.max.y || p.z > box.max.z) return false;
    cfloat3 g = (p - origin_) * (1.0f / cell_size_);
    int bx = std::clamp(static_cast<int>(std::floor(g.x)), 0, bcount_[0] * kBrickDim - 1) / kBrickDim;
    int by = std::clamp(static_cast<int>(std::floor(g.y)), 0, bcount_[1] * kBrickDim - 1) / kBrickDim;
    int bz = std::clamp(static_cast<int>(std::floor(g.z)), 0, bcount_[2] * kBrickDim - 1) / kBrickDim;
    return index_[static_cast<std::size_t>((bz * bcount_[1] + by) * bcount_[0] + bx)] >= 0;
}

// The field at a point known to lie inside the sampled box.
float FieldVolume::eval_inside(cfloat3 p) const {
    cfloat3 g = (p - origin_) * (1.0f / cell_size_);
    int cx = static_cast<int>(std::floor(g.x));
    int cy = static_cast<int>(std::floor(g.y));
    int cz = static_cast<int>(std::floor(g.z));
    cx = std::clamp(cx, 0, bcount_[0] * kBrickDim - 1);
    cy = std::clamp(cy, 0, bcount_[1] * kBrickDim - 1);
    cz = std::clamp(cz, 0, bcount_[2] * kBrickDim - 1);

    int bx = cx / kBrickDim, by = cy / kBrickDim, bz = cz / kBrickDim;
    std::size_t slot = static_cast<std::size_t>((bz * bcount_[1] + by) * bcount_[0] + bx);
    std::int32_t entry = index_[slot];
    if (entry == kBrickEmpty) return far_[slot];

    const float* block = data_.data() + entry;
    int lx = cx - bx * kBrickDim, ly = cy - by * kBrickDim, lz = cz - bz * kBrickDim;
    float fx = std::clamp(g.x - static_cast<float>(cx), 0.0f, 1.0f);
    float fy = std::clamp(g.y - static_cast<float>(cy), 0.0f, 1.0f);
    float fz = std::clamp(g.z - static_cast<float>(cz), 0.0f, 1.0f);

    auto at = [&](int x, int y, int z) { return block[sample_index(lx + x, ly + y, lz + z)]; };
    float c00 = at(0, 0, 0) * (1 - fx) + at(1, 0, 0) * fx;
    float c10 = at(0, 1, 0) * (1 - fx) + at(1, 1, 0) * fx;
    float c01 = at(0, 0, 1) * (1 - fx) + at(1, 0, 1) * fx;
    float c11 = at(0, 1, 1) * (1 - fx) + at(1, 1, 1) * fx;
    float c0 = c00 * (1 - fy) + c10 * fy;
    float c1 = c01 * (1 - fy) + c11 * fy;
    return c0 * (1 - fz) + c1 * fz;
}

float FieldVolume::eval(cfloat3 p) const {
    if (empty()) return 3.4e38f;
    math::Aabb box = bounds();
    cfloat3 c = cf3(std::clamp(p.x, box.min.x, box.max.x), std::clamp(p.y, box.min.y, box.max.y),
                    std::clamp(p.z, box.min.z, box.max.z));
    float outside = kernel::clength(p - c);
    float inner = eval_inside(c);
    if (outside <= 0.0f) return inner;

    // Outside the sampled box, the distance to the box ALONE is not usable: it
    // falls to zero on the box face, and a sphere tracer reads zero as a
    // surface — every ray would hit an invisible shell where the sampling
    // stopped. What the face is worth is how far the real surface is from it,
    // which is exactly what the field at the clamped point says.
    //
    // Combining them by Pythagoras is not an approximation. c is the
    // projection of p onto the box, so (p - c) . (s - c) <= 0 for every point
    // s in the box, and hence |p - s|^2 >= |p - c|^2 + |c - s|^2. Taking the
    // positive part of `inner` covers the case where a caller-supplied box
    // CLIPS the solid: the zero set then genuinely includes the box face, and
    // the distance to the box is the exact answer.
    float reach = std::max(inner, 0.0f);
    return std::sqrt(outside * outside + reach * reach);
}

// -- rewriting the samples ----------------------------------------------------

namespace {

// Where a global cell coordinate lives, per axis. A coordinate on a brick face
// belongs to TWO bricks — the top of the lower one and the bottom of the upper
// — so it can have two answers, and each axis decides independently. Trying to
// share one "prefer the lower" flag across all three axes lands on the
// diagonal neighbour instead of the four bricks that actually share the face.
struct AxisSlot {
    int brick[2];
    int local[2];
    int count;
};

AxisSlot locate(int g, int bcount) {
    AxisSlot slot{{0, 0}, {0, 0}, 0};
    if (g < 0 || g > bcount * kBrickDim) return slot;
    int b = g / kBrickDim;
    int l = g - b * kBrickDim;
    if (b < bcount) {
        slot.brick[slot.count] = b;
        slot.local[slot.count] = l;
        ++slot.count;
    }
    if (l == 0 && b > 0) {  // the same sample, as the brick below's halo
        slot.brick[slot.count] = b - 1;
        slot.local[slot.count] = kBrickDim;
        ++slot.count;
    }
    return slot;
}

}  // namespace

std::optional<float> FieldVolume::sample_at(int gx, int gy, int gz) const {
    if (empty()) return std::nullopt;
    AxisSlot sx = locate(gx, bcount_[0]);
    AxisSlot sy = locate(gy, bcount_[1]);
    AxisSlot sz = locate(gz, bcount_[2]);
    // Up to eight bricks share a sample on a corner; any that stores it answers,
    // and they agree because the halo holds the same value.
    for (int iz = 0; iz < sz.count; ++iz)
        for (int iy = 0; iy < sy.count; ++iy)
            for (int ix = 0; ix < sx.count; ++ix) {
                std::size_t slot = static_cast<std::size_t>(
                    (sz.brick[iz] * bcount_[1] + sy.brick[iy]) * bcount_[0] + sx.brick[ix]);
                std::int32_t entry = index_[slot];
                if (entry < 0) continue;
                return data_[static_cast<std::size_t>(entry) +
                             sample_index(sx.local[ix], sy.local[iy], sz.local[iz])];
            }
    return std::nullopt;
}

void FieldVolume::rewrite(const std::function<float(int, int, int, float)>& fn) {
    const int n = kBrickDim + 1;
    for (std::size_t slot = 0; slot < index_.size(); ++slot) {
        std::int32_t entry = index_[slot];
        if (entry < 0) continue;
        const int bx = static_cast<int>(slot) % bcount_[0];
        const int by = (static_cast<int>(slot) / bcount_[0]) % bcount_[1];
        const int bz = static_cast<int>(slot) / (bcount_[0] * bcount_[1]);
        for (int i = 0; i < kBrickSamples; ++i) {
            const int lx = i % n, ly = (i / n) % n, lz = i / (n * n);
            std::size_t at = static_cast<std::size_t>(entry) + static_cast<std::size_t>(i);
            data_[at] =
                fn(bx * kBrickDim + lx, by * kBrickDim + ly, bz * kBrickDim + lz, data_[at]);
        }
    }
}

void FieldVolume::rewrite_region(const math::Aabb& region,
                                 const std::function<float(int, int, int, float)>& fn) {
    if (empty() || region.empty()) return;
    const int n = kBrickDim + 1;
    const float brick = static_cast<float>(kBrickDim) * cell_size_;

    // Brick b spans world [origin + b*brick, origin + (b+1)*brick] along an
    // axis — the closing face is the halo sample, shared with b+1 — so a brick
    // meets the region when its span overlaps it. Rounded OUTWARD by one brick
    // on each side: a brick too many costs a brick of work, and the whole
    // argument for skipping the rest is that `fn` is identity there, which a
    // generous selection cannot break.
    int lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        const float o = a == 0 ? origin_.x : a == 1 ? origin_.y : origin_.z;
        const float rmin = a == 0 ? region.min.x : a == 1 ? region.min.y : region.min.z;
        const float rmax = a == 0 ? region.max.x : a == 1 ? region.max.y : region.max.z;
        lo[a] = static_cast<int>(std::floor((rmin - o) / brick)) - 1;
        hi[a] = static_cast<int>(std::floor((rmax - o) / brick)) + 1;
        lo[a] = std::max(lo[a], 0);
        hi[a] = std::min(hi[a], bcount_[a] - 1);
    }
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return;  // nothing meets it

    for (int bz = lo[2]; bz <= hi[2]; ++bz)
        for (int by = lo[1]; by <= hi[1]; ++by)
            for (int bx = lo[0]; bx <= hi[0]; ++bx) {
                const std::size_t slot =
                    static_cast<std::size_t>((bz * bcount_[1] + by) * bcount_[0] + bx);
                const std::int32_t entry = index_[slot];
                if (entry < 0) continue;
                for (int i = 0; i < kBrickSamples; ++i) {
                    const int lx = i % n, ly = (i / n) % n, lz = i / (n * n);
                    const std::size_t at =
                        static_cast<std::size_t>(entry) + static_cast<std::size_t>(i);
                    data_[at] = fn(bx * kBrickDim + lx, by * kBrickDim + ly,
                                   bz * kBrickDim + lz, data_[at]);
                }
            }
}

float FieldVolume::measure_sample_lipschitz() const {
    if (empty()) return 1.0f;
    const int n = kBrickDim + 1;

    // Walking the STORED bricks rather than the dense lattice. The two see
    // exactly the same pairs, and the reason is the halo: a forward pair
    // (g, g+1) lies wholly inside the one brick g/8 — at locals (g%8, g%8+1),
    // and g%8 is at most kBrickDim-1 so the upper end is at worst the halo
    // sample. If that brick has no samples then neither does the upper end,
    // because the only OTHER brick holding a sample is the one below, and it
    // holds it at its halo, which is the lower end of the pair and never the
    // upper. So a pair the dense sweep counted is a pair some stored brick
    // owns, and a pair a stored brick owns is one the dense sweep counted.
    //
    // What it saves is not arithmetic, it is lookups. The dense sweep asked
    // sample_at() for every point of the bounding lattice — 8.1M of them for a
    // volume storing 1.55M samples in 2,132 of 15,625 brick slots — and each
    // one is a sparse localize plus up to eight brick probes. Six million of
    // those returned nothing. Inside a brick the pairs are neighbours in
    // memory, so this asks for nothing at all.
    //
    // Halo duplicates cannot disagree, which is what lets a pair be settled
    // inside one brick: both copies of a shared sample come from the same
    // global coordinate through the same arithmetic — sample_position builds
    // it as bx * kBrickDim + x, so brick b's halo and brick b+1's face are the
    // same integer — and rewrite() hands both copies the same question.
    constexpr int kStride[3] = {1, kBrickDim + 1, (kBrickDim + 1) * (kBrickDim + 1)};

    float steepest = 0.0f;
    for (std::size_t slot = 0; slot < index_.size(); ++slot) {
        const std::int32_t entry = index_[slot];
        if (entry < 0) continue;
        const float* block = data_.data() + static_cast<std::size_t>(entry);
        // One sweep per axis, each stopping a sample short along that axis so
        // the forward neighbour is always in the block. Only the three forward
        // neighbours: the reverse ones are the same pairs seen from the other
        // end.
        for (int axis = 0; axis < 3; ++axis) {
            const int stride = kStride[axis];
            const int last[3] = {axis == 0 ? n - 1 : n, axis == 1 ? n - 1 : n,
                                 axis == 2 ? n - 1 : n};
            for (int z = 0; z < last[2]; ++z)
                for (int y = 0; y < last[1]; ++y) {
                    const float* row = block + (z * n + y) * n;
                    for (int x = 0; x < last[0]; ++x)
                        steepest = std::max(steepest, std::abs(row[x + stride] - row[x]));
                }
        }
    }
    return std::max(1.0f, steepest / cell_size_);
}

std::size_t FieldVolume::compact() {
    if (data_.empty()) return 0;
    std::vector<float> kept;
    kept.reserve(data_.size());
    std::size_t dropped = 0;
    for (std::size_t slot = 0; slot < index_.size(); ++slot) {
        const std::int32_t entry = index_[slot];
        if (entry < 0) continue;
        const float* block = data_.data() + static_cast<std::size_t>(entry);
        bool near_surface = false, any_inside = false;
        for (int i = 0; i < kBrickSamples; ++i) {
            if (std::abs(block[i]) <= band_) near_surface = true;
            if (block[i] < 0.0f) any_inside = true;
        }
        if (near_surface) {
            index_[slot] = static_cast<std::int32_t>(kept.size());
            kept.insert(kept.end(), block, block + kBrickSamples);
            continue;
        }
        // Sign only, exactly as sample() records it; build_far_bounds supplies
        // the magnitude from the brick's distance to the nearest one kept.
        index_[slot] = kBrickEmpty;
        far_[slot] = any_inside ? -1.0f : 1.0f;
        ++dropped;
    }
    data_ = std::move(kept);
    build_far_bounds();
    return dropped;
}

void FieldVolume::shrink_band(float by) {
    if (!(by > 0.0f)) return;
    // Never below the sample spacing: a band thinner than that says nothing the
    // samples do not already, and drives far_value() to zero, which stalls a
    // marcher rather than stepping it.
    band_ = std::max(band_ - by, cell_size_ * 2.0f);
    build_far_bounds();
}

// -- blob layout --------------------------------------------------------------
// [0..2] origin  [3] cell size  [4] band  [5..7] brick counts
// [8] index offset  [9] far-bound offset  [10] data offset, all absolute
// within this block. The index and the far bounds are one entry per brick and
// are read together; the samples follow.
//
// [11] sample Lipschitz, present only when the index offset says the header is
// that long. The header SIZE is the index offset, so the layout describes its
// own length: a reader that predates this field sees an index offset of 11 and
// finds its three offsets exactly where it always did, and a reader that
// postdates it can tell the two apart without a version number to keep in
// step with anything else.
//
// [12] feather (add-feathered-volume-replace), by the same self-describing
// rule: absent reads as 0, which is the hard replace.

// The header, then one index entry and one far bound per brick, then the
// samples. Kept beside to_blob so the two cannot drift.
std::size_t FieldVolume::blob_floats() const {
    // Must track to_blob exactly: a host sizes an upload from this and a
    // disagreement is a truncated buffer rather than a wrong number.
    return 14 + index_.size() + far_.size() + data_.size() + colors_.size();
}

std::vector<float> FieldVolume::to_blob() const {
    std::vector<float> out;
    // Slot 13 is the colour offset, 0 when this volume has none. Adding it to
    // the END of the header is what keeps an old reader working: every section
    // is addressed by the offsets in slots 8..10, which are computed from this
    // header size, so a reader that stops at slot 12 finds the same index, far
    // and data arrays it always did and simply never looks for colour.
    const std::size_t header = 14;
    out.resize(header);
    out[0] = origin_.x;
    out[1] = origin_.y;
    out[2] = origin_.z;
    out[3] = cell_size_;
    out[4] = band_;
    out[5] = static_cast<float>(bcount_[0]);
    out[6] = static_cast<float>(bcount_[1]);
    out[7] = static_cast<float>(bcount_[2]);
    out[8] = static_cast<float>(header);
    out[9] = static_cast<float>(header + index_.size());
    out[10] = static_cast<float>(header + index_.size() + far_.size());
    out[11] = sample_lipschitz_;
    out[12] = feather_;
    out[13] = colors_.empty()
                  ? 0.0f
                  : static_cast<float>(header + index_.size() + far_.size() + data_.size());
    for (std::int32_t e : index_) out.push_back(static_cast<float>(e));
    out.insert(out.end(), far_.begin(), far_.end());
    out.insert(out.end(), data_.begin(), data_.end());
    // Packed 0x00RRGGBB as a float VALUE rather than as reinterpreted bits.
    // The packed colour is at most 2^24 - 1, and float32 represents every
    // integer up to 2^24 exactly, so this round-trips without a bit-cast —
    // which matters because the kernel dialect would need a different one per
    // backend and this needs none.
    for (std::uint32_t c : colors_) out.push_back(static_cast<float>(c));
    return out;
}

std::optional<FieldVolume> FieldVolume::from_blob(const std::vector<float>& blob) {
    if (blob.size() < 11) return std::nullopt;
    FieldVolume v;
    v.origin_ = cf3(blob[0], blob[1], blob[2]);
    v.cell_size_ = blob[3];
    v.band_ = blob[4];
    for (int a = 0; a < 3; ++a) v.bcount_[a] = static_cast<std::int32_t>(blob[5 + a]);
    if (!(v.cell_size_ > 0.0f) || v.bcount_[0] <= 0 || v.bcount_[1] <= 0 || v.bcount_[2] <= 0)
        return std::nullopt;
    std::size_t index_off = static_cast<std::size_t>(blob[8]);
    std::size_t far_off = static_cast<std::size_t>(blob[9]);
    std::size_t data_off = static_cast<std::size_t>(blob[10]);
    // The header size IS the index offset, so a blob written before the
    // Lipschitz field simply does not have one and reads as 1, and one
    // written before the feather reads as 0 — the hard replace.
    v.sample_lipschitz_ = index_off > 11 ? std::max(blob[11], 1.0f) : 1.0f;
    v.feather_ = index_off > 12 ? std::max(blob[12], 0.0f) : 0.0f;
    std::size_t index_size =
        static_cast<std::size_t>(v.bcount_[0]) * v.bcount_[1] * v.bcount_[2];
    if (index_off + index_size > blob.size() || far_off + index_size > blob.size() ||
        data_off > blob.size())
        return std::nullopt;
    // Each index entry is an offset into the sample data that eval_inside and
    // the tape both read kBrickSamples floats from. Checking the offsets that
    // bound the sections is not enough: one entry pointing past the samples is
    // an arbitrary-offset read, and a blob off a disk carries any value.
    const std::size_t data_size = blob.size() - data_off;
    v.index_.reserve(index_size);
    for (std::size_t i = 0; i < index_size; ++i) {
        const float raw = blob[index_off + i];
        if (raw < 0.0f) {
            // kBrickEmpty is the one negative value that is legal.
            if (static_cast<std::int32_t>(raw) != kBrickEmpty) return std::nullopt;
            v.index_.push_back(kBrickEmpty);
            continue;
        }
        // Entries are offsets into the sample data, and every stored brick
        // starts on a kBrickSamples boundary. Past 2^24 a float can no longer
        // hold consecutive integers, so a large volume reads its own offsets
        // back rounded — recovering the boundary is what makes the bound below
        // exact rather than off by the float's spacing.
        const std::int64_t units =
            static_cast<std::int64_t>(std::llround(raw / static_cast<double>(kBrickSamples)));
        const std::int64_t e = units * kBrickSamples;
        if (e < 0 || static_cast<std::size_t>(e) + kBrickSamples > data_size) return std::nullopt;
        v.index_.push_back(static_cast<std::int32_t>(e));
    }
    v.far_.assign(blob.begin() + static_cast<std::ptrdiff_t>(far_off),
                  blob.begin() + static_cast<std::ptrdiff_t>(far_off + index_size));

    // Where the samples END. Slot 8 IS the header size, so it says whether
    // this blob was written with a colour slot at all: a blob from before the
    // colour channel has a 13-float header and no slot 13, and reading one
    // there would read its first index entry as an offset.
    const std::size_t header_size = index_off;
    std::size_t colors_off = 0;
    if (header_size >= 14) colors_off = static_cast<std::size_t>(blob[13]);
    if (colors_off != 0 && (colors_off < data_off || colors_off > blob.size())) return std::nullopt;

    const std::size_t data_end = colors_off != 0 ? colors_off : blob.size();
    v.data_.assign(blob.begin() + static_cast<std::ptrdiff_t>(data_off),
                   blob.begin() + static_cast<std::ptrdiff_t>(data_end));
    if (colors_off != 0) {
        // One colour per stored sample or none; a mismatch means a truncated
        // or forged blob, and reading it would index past the samples.
        if (blob.size() - colors_off != v.data_.size()) return std::nullopt;
        v.colors_.reserve(v.data_.size());
        for (std::size_t i = colors_off; i < blob.size(); ++i) {
            const float raw = blob[i];
            if (!(raw >= 0.0f) || raw > 16777215.0f) return std::nullopt;
            v.colors_.push_back(static_cast<std::uint32_t>(raw));
        }
    }
    return v;
}

std::vector<std::uint8_t> FieldVolume::serialize(bool with_color) const {
    std::vector<float> flat;
    if (with_color || colors_.empty()) {
        flat = to_blob();
    } else {
        // The same volume without its colour, by asking a copy that has none.
        // Cheaper than a second blob writer, and it cannot drift from one.
        FieldVolume plain = *this;
        plain.colors_.clear();
        flat = plain.to_blob();
    }
    std::vector<std::uint8_t> out(flat.size() * sizeof(float));
    if (!flat.empty()) std::memcpy(out.data(), flat.data(), out.size());
    return out;
}

std::optional<FieldVolume> FieldVolume::deserialize(const std::uint8_t* data, std::size_t size) {
    if (size % sizeof(float) != 0) return std::nullopt;
    std::vector<float> flat(size / sizeof(float));
    if (size) std::memcpy(flat.data(), data, size);
    return from_blob(flat);
}

}  // namespace field
}  // namespace clay
