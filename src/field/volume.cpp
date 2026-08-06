// Sampled fields (sdf-kernels spec, add-sampled-fields). See
// include/clay/field/volume.h for why a narrow band rides in the tape's blob
// rather than behind a resource handle.

#include "clay/field/volume.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clay {
namespace field {

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

// What sampling one brick found out about it.
struct BrickScan {
    bool near_surface;  // something landed within the band: the brick is kept
    bool any_inside;    // ...and if not, which side the whole of it is on
};

// Sample one brick, halo included, into `block`.
//
// The three axes are walked as ONE index rather than three nested loops. That
// is not a micro-optimisation: sample_index(x, y, z) is exactly that linear
// index, so the nesting bought nothing except depth.
BrickScan scan_brick(const std::function<float(cfloat3)>& f, cfloat3 origin, float cell,
                     float band, int bx, int by, int bz, std::vector<float>& block) {
    BrickScan scan{false, false};
    const int n = kBrickDim + 1;
    for (int i = 0; i < kBrickSamples; ++i) {
        int x = i % n, y = (i / n) % n, z = i / (n * n);
        cfloat3 p = origin + cf3(static_cast<float>(bx * kBrickDim + x),
                                 static_cast<float>(by * kBrickDim + y),
                                 static_cast<float>(bz * kBrickDim + z)) *
                                cell;
        float d = f(p);
        block[static_cast<std::size_t>(i)] = d;
        if (std::abs(d) <= band) scan.near_surface = true;
        if (d < 0.0f) scan.any_inside = true;
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

FieldVolume FieldVolume::sample(const std::function<float(cfloat3)>& f, const math::Aabb& region,
                                float cell_size, float band) {
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
    // volume in memory for a region that is mostly empty by construction.
    std::vector<float> block(kBrickSamples);
    for (std::size_t slot = 0; slot < total; ++slot) {
        const int bx = static_cast<int>(slot) % v.bcount_[0];
        const int by = (static_cast<int>(slot) / v.bcount_[0]) % v.bcount_[1];
        const int bz = static_cast<int>(slot) / (v.bcount_[0] * v.bcount_[1]);
        BrickScan scan = scan_brick(f, v.origin_, v.cell_size_, v.band_, bx, by, bz, block);
        if (scan.near_surface) {
            v.index_[slot] = static_cast<std::int32_t>(v.data_.size());
            v.data_.insert(v.data_.end(), block.begin(), block.end());
        } else {
            // No samples: the whole brick is on one side, and which side is
            // the thing a band alone could not tell us. The magnitude comes
            // from build_far_bounds; this only records the sign.
            v.far_[slot] = scan.any_inside ? -1.0f : 1.0f;
        }
    }
    v.build_far_bounds();
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

// -- blob layout --------------------------------------------------------------
// [0..2] origin  [3] cell size  [4] band  [5..7] brick counts
// [8] index offset  [9] far-bound offset  [10] data offset, all absolute
// within this block. The index and the far bounds are one entry per brick and
// are read together; the samples follow.

std::vector<float> FieldVolume::to_blob() const {
    std::vector<float> out;
    const std::size_t header = 11;
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
    for (std::int32_t e : index_) out.push_back(static_cast<float>(e));
    out.insert(out.end(), far_.begin(), far_.end());
    out.insert(out.end(), data_.begin(), data_.end());
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
    std::size_t index_size =
        static_cast<std::size_t>(v.bcount_[0]) * v.bcount_[1] * v.bcount_[2];
    if (index_off + index_size > blob.size() || far_off + index_size > blob.size() ||
        data_off > blob.size())
        return std::nullopt;
    v.index_.reserve(index_size);
    for (std::size_t i = 0; i < index_size; ++i)
        v.index_.push_back(static_cast<std::int32_t>(blob[index_off + i]));
    v.far_.assign(blob.begin() + static_cast<std::ptrdiff_t>(far_off),
                  blob.begin() + static_cast<std::ptrdiff_t>(far_off + index_size));
    v.data_.assign(blob.begin() + static_cast<std::ptrdiff_t>(data_off), blob.end());
    return v;
}

std::vector<std::uint8_t> FieldVolume::serialize() const {
    std::vector<float> flat = to_blob();
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
