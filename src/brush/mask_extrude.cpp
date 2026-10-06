// Mask extrude (sdf-kernels and voxel-engine specs, add-mask-extrude). See
// include/clay/field/mask_extrude.h for why the only new mechanism here is
// measuring a mask as a distance, and why the mask needs no region parameter.

#include "clay/brush/mask_extrude.h"
#include "mask_extrude_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "clay/kernel/ops.h"

namespace clay {
namespace brush {

using kernel::cf3;
using kernel::cfloat3;
using voxel::VoxelCoord;

namespace {

constexpr float kInf = std::numeric_limits<float>::max() * 0.25f;

// -- exact Euclidean distance transform ---------------------------------------
//
// Felzenszwalb & Huttenlocher's lower-envelope-of-parabolas transform, one axis
// at a time. Exact rather than a chamfer approximation, which matters because
// the result becomes a DISTANCE FIELD: a chamfer's error is anisotropic, so its
// isosurface has flats where the lattice does and a rim extruded from it would
// show them.

void transform_1d(std::vector<float>& f, std::vector<int>& v, std::vector<float>& z,
                  std::vector<float>& d) {
    const int n = static_cast<int>(f.size());
    const auto crossing = [&f](int q, int p) {
        const float fq = f[q] + static_cast<float>(q) * static_cast<float>(q);
        const float fp = f[p] + static_cast<float>(p) * static_cast<float>(p);
        return (fq - fp) / static_cast<float>(2 * (q - p));
    };
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        // k never falls below zero because z[0] is -kInf and a crossing is
        // finite, which is the whole reason the sentinel is there.
        float s = crossing(q, v[k]);
        while (s <= z[k]) {
            --k;
            s = crossing(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) ++k;
        const float dq = static_cast<float>(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
    std::copy(d.begin(), d.end(), f.begin());
}

// Squared distance in CELLS from every cell to the nearest cell where `seed` is
// true. Cells outside the array are not seeds, which is why the caller pads.
std::vector<float> squared_edt(const std::vector<std::uint8_t>& seed, int nx, int ny, int nz) {
    std::vector<float> f(seed.size());
    for (std::size_t i = 0; i < seed.size(); ++i) f[i] = seed[i] ? 0.0f : kInf;
    const auto at = [nx, ny](int x, int y, int z) {
        return (static_cast<std::size_t>(z) * ny + y) * nx + x;
    };

    const int longest = std::max({nx, ny, nz});
    std::vector<float> line(longest), d(longest);
    std::vector<int> v(longest);
    std::vector<float> z(longest + 1);

    const auto run = [&](int count, auto index) {
        line.resize(count);
        d.resize(count);
        v.resize(count);
        z.resize(count + 1);
        for (int i = 0; i < count; ++i) line[i] = f[index(i)];
        transform_1d(line, v, z, d);
        for (int i = 0; i < count; ++i) f[index(i)] = line[i];
    };

    for (int z0 = 0; z0 < nz; ++z0)
        for (int y = 0; y < ny; ++y) run(nx, [&](int x) { return at(x, y, z0); });
    for (int z0 = 0; z0 < nz; ++z0)
        for (int x = 0; x < nx; ++x) run(ny, [&](int y) { return at(x, y, z0); });
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) run(nz, [&](int z0) { return at(x, y, z0); });
    return f;
}

// -- the mask, measured -------------------------------------------------------
//
// A dense signed distance over the padded mask region, trilinearly sampled.
// Deliberately NOT a FieldVolume internally: a volume reports a flat bound
// outside its band, and composing that into the extrude would bake the seam
// between bound and distance into the result — the defect flatten's header
// warns about. The dense array is a real distance everywhere in its box.
struct MaskDistance {
    VoxelCoord lo{0, 0, 0};
    int nx = 0, ny = 0, nz = 0;
    float cell = 0.0f;
    std::vector<float> d;  // signed, world units, at cell centres

    math::Aabb bounds() const {
        math::Aabb b;
        b.min = cf3(static_cast<float>(lo.x), static_cast<float>(lo.y),
                    static_cast<float>(lo.z)) *
                cell;
        b.max = cf3(static_cast<float>(lo.x + nx), static_cast<float>(lo.y + ny),
                    static_cast<float>(lo.z + nz)) *
                cell;
        return b;
    }

    float eval(cfloat3 p) const {
        // Cell centres sit at (index + 0.5) cells, so the sample lattice is
        // offset half a cell from the box.
        const float fx = p.x / cell - 0.5f - static_cast<float>(lo.x);
        const float fy = p.y / cell - 0.5f - static_cast<float>(lo.y);
        const float fz = p.z / cell - 0.5f - static_cast<float>(lo.z);
        const auto axis = [](float f, int n, int* i0, int* i1, float* t) {
            const float clamped = std::clamp(f, 0.0f, static_cast<float>(n - 1));
            *i0 = static_cast<int>(std::floor(clamped));
            *i1 = std::min(*i0 + 1, n - 1);
            *t = clamped - static_cast<float>(*i0);
        };
        int x0, x1, y0, y1, z0, z1;
        float tx, ty, tz;
        axis(fx, nx, &x0, &x1, &tx);
        axis(fy, ny, &y0, &y1, &ty);
        axis(fz, nz, &z0, &z1, &tz);
        const auto at = [&](int x, int y, int z) {
            return d[(static_cast<std::size_t>(z) * ny + y) * nx + x];
        };
        const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        const float c00 = lerp(at(x0, y0, z0), at(x1, y0, z0), tx);
        const float c10 = lerp(at(x0, y1, z0), at(x1, y1, z0), tx);
        const float c01 = lerp(at(x0, y0, z1), at(x1, y0, z1), tx);
        const float c11 = lerp(at(x0, y1, z1), at(x1, y1, z1), tx);
        return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
    }
};

std::optional<MaskDistance> measure(const voxel::MaskField& mask, float threshold, float pad,
                                    float cell) {
    std::optional<VoxelCoord> lo = mask.bounds_min(), hi = mask.bounds_max();
    if (!lo || !hi) return std::nullopt;

    // The mask's lattice and the requested one need not be the same, so the
    // dense array is laid out on the REQUESTED cell size and the mask is sampled
    // into it. That is what lets an extract be sampled finer than the paint.
    const float pad_cells = std::ceil(pad / cell) + 1.0f;
    const auto to_cell = [cell](float world) {
        return static_cast<std::int32_t>(std::floor(world / cell));
    };
    const float ms = mask.cell_size();
    MaskDistance md;
    md.cell = cell;
    md.lo = {to_cell(static_cast<float>(lo->x) * ms) - static_cast<std::int32_t>(pad_cells),
             to_cell(static_cast<float>(lo->y) * ms) - static_cast<std::int32_t>(pad_cells),
             to_cell(static_cast<float>(lo->z) * ms) - static_cast<std::int32_t>(pad_cells)};
    const VoxelCoord end{to_cell(static_cast<float>(hi->x + 1) * ms) +
                             static_cast<std::int32_t>(pad_cells),
                         to_cell(static_cast<float>(hi->y + 1) * ms) +
                             static_cast<std::int32_t>(pad_cells),
                         to_cell(static_cast<float>(hi->z + 1) * ms) +
                             static_cast<std::int32_t>(pad_cells)};
    md.nx = end.x - md.lo.x + 1;
    md.ny = end.y - md.lo.y + 1;
    md.nz = end.z - md.lo.z + 1;
    if (md.nx <= 0 || md.ny <= 0 || md.nz <= 0) return std::nullopt;

    const std::size_t count =
        static_cast<std::size_t>(md.nx) * static_cast<std::size_t>(md.ny) *
        static_cast<std::size_t>(md.nz);
    std::vector<std::uint8_t> inside(count, 0), outside(count, 0);
    bool any_inside = false;
    for (int z = 0; z < md.nz; ++z)
        for (int y = 0; y < md.ny; ++y)
            for (int x = 0; x < md.nx; ++x) {
                const cfloat3 centre =
                    cf3(static_cast<float>(md.lo.x + x) + 0.5f,
                        static_cast<float>(md.lo.y + y) + 0.5f,
                        static_cast<float>(md.lo.z + z) + 0.5f) *
                    cell;
                const bool in = mask.sample(centre) >= threshold;
                const std::size_t i = (static_cast<std::size_t>(z) * md.ny + y) * md.nx + x;
                inside[i] = in ? 1 : 0;
                outside[i] = in ? 0 : 1;
                any_inside = any_inside || in;
            }
    // A mask painted below the threshold everywhere describes no region, and a
    // distance field to an empty set is not a thing to hand back.
    if (!any_inside) return std::nullopt;

    const std::vector<float> to_inside = squared_edt(inside, md.nx, md.ny, md.nz);
    const std::vector<float> to_outside = squared_edt(outside, md.nx, md.ny, md.nz);
    md.d.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Half a cell off the cell-centre measurement, because the boundary lies
        // BETWEEN a masked cell and its unmasked neighbour rather than on either.
        const float outward = std::sqrt(to_inside[i]) - 0.5f;
        const float inward = std::sqrt(to_outside[i]) - 0.5f;
        md.d[i] = (inside[i] ? -inward : outward) * cell;
    }
    return md;
}

float shell_of(float d, ExtrudeSide side, float thickness) {
    switch (side) {
        case ExtrudeSide::Inward:
            return std::max(d, -d - thickness);
        case ExtrudeSide::Centred:
            return std::abs(d) - thickness * 0.5f;
        case ExtrudeSide::Outward:
        default:
            return std::max(-d, d - thickness);
    }
}

// Where both extrudes agree on how many cells a thickness is, so the SDF and
// voxel paths cannot drift apart on rounding alone.
int layers_for(float thickness, float cell) {
    return std::max(static_cast<int>(std::lround(thickness / cell)), 1);
}

// A painted mask describes a patch ON the source, not a volume that must
// itself extend through the requested wall. Read it at the nearest point on
// the source surface so the wall keeps the same footprint at every height.
// `gradient` need only point the right way; its length is divided out.
cfloat3 project_to_surface(cfloat3 p, cfloat3 gradient, float distance) {
    const float length = kernel::clength(gradient);
    if (length < 1e-6f) return p;
    return p - gradient * (distance / length);
}

// -- where the mask cannot matter ---------------------------------------------
//
// The extrude stores max(shell, region), or the quadratic smooth max, which
// equals max(shell, region) wherever the two are at least its support apart.
// `region` is the mask read at the sample's projection onto the source, which
// costs a gradient and a trilinear read per sample. Most samples do not need
// it: where the shell already exceeds anything the
// region could be, the stored value IS the shell, bit for bit, whatever the
// projection would have said. These pieces decide that without projecting.

// The most MaskDistance::eval can return at any point within a given reach of
// a sample, from a max pyramid over the dense distances. Conservative by
// construction: eval is a convex combination of eight stored distances, and
// the pyramid answers with the largest stored distance over a box containing
// every corner eval could read.
class RegionCeiling {
  public:
    explicit RegionCeiling(const MaskDistance& md) : md_(md) {
        float largest = 0.0f;
        for (float v : md.d) largest = std::max(largest, std::abs(v));
        // Trilinear interpolation in float can land a few ulps above its
        // largest corner; this covers it with two orders of magnitude to spare.
        slack_ = largest * 1e-5f;
        // Level 0 IS the dense distances, read in place rather than copied:
        // this verb has been killed for memory on the tablet, and a copy would
        // double the largest array it holds while sampling. The coarser levels
        // together add about a seventh of it.
        levels_.push_back(Level{md.nx, md.ny, md.nz, md.d.data()});
        while (levels_.back().nx > 1 || levels_.back().ny > 1 || levels_.back().nz > 1) {
            const Level fine = levels_.back();
            Level coarse{(fine.nx + 1) / 2, (fine.ny + 1) / 2, (fine.nz + 1) / 2, nullptr};
            // A vector's buffer survives the outer vector growing, so the
            // pointer taken here stays valid.
            owned_.push_back(halve(fine, coarse));
            coarse.m = owned_.back().data();
            levels_.push_back(coarse);
        }
    }

    // An upper bound on md.eval(q) for every q within `reach` of p. Infinite
    // when the query cannot be placed, which makes the caller evaluate.
    float above(cfloat3 p, float reach) const {
        // In cells, as eval measures them, widened past the rounding in both
        // the projection and the index arithmetic.
        const float r = reach / md_.cell * 1.001f + 0.01f;
        int x0, x1, y0, y1, z0, z1;
        if (!span(p.x / md_.cell - 0.5f - static_cast<float>(md_.lo.x), r, md_.nx, &x0, &x1) ||
            !span(p.y / md_.cell - 0.5f - static_cast<float>(md_.lo.y), r, md_.ny, &y0, &y1) ||
            !span(p.z / md_.cell - 0.5f - static_cast<float>(md_.lo.z), r, md_.nz, &z0, &z1))
            return std::numeric_limits<float>::infinity();
        // The finest level at which the span covers at most two entries a side.
        int k = 0;
        while (((x1 >> k) - (x0 >> k)) > 1 || ((y1 >> k) - (y0 >> k)) > 1 ||
               ((z1 >> k) - (z0 >> k)) > 1)
            ++k;
        const Level& level = levels_[static_cast<std::size_t>(k)];
        float most = -std::numeric_limits<float>::infinity();
        for (int z = z0 >> k; z <= z1 >> k; ++z)
            for (int y = y0 >> k; y <= y1 >> k; ++y)
                for (int x = x0 >> k; x <= x1 >> k; ++x) most = std::max(most, level.at(x, y, z));
        return most + slack_;
    }

  private:
    // One level of the pyramid, as a view: level 0 points into the
    // MaskDistance, the rest into owned_.
    struct Level {
        int nx, ny, nz;
        const float* m;
        float at(int x, int y, int z) const {
            return m[(static_cast<std::size_t>(z) * ny + y) * nx + x];
        }
    };

    // The maxima of `fine` over 2x2x2 blocks, laid out as `coarse` describes.
    static std::vector<float> halve(const Level& fine, const Level& coarse) {
        std::vector<float> m(static_cast<std::size_t>(coarse.nx) * coarse.ny * coarse.nz,
                             -std::numeric_limits<float>::infinity());
        for (int z = 0; z < fine.nz; ++z)
            for (int y = 0; y < fine.ny; ++y)
                for (int x = 0; x < fine.nx; ++x) {
                    const std::size_t at =
                        (static_cast<std::size_t>(z / 2) * coarse.ny + y / 2) * coarse.nx + x / 2;
                    m[at] = std::max(m[at], fine.at(x, y, z));
                }
        return m;
    }

    // The lattice indices eval's corners can take for a coordinate anywhere in
    // [f - r, f + r], clamped exactly as eval clamps.
    static bool span(float f, float r, int n, int* lo, int* hi) {
        const float a = f - r, b = f + r;
        if (!std::isfinite(a) || !std::isfinite(b)) return false;
        const float top = static_cast<float>(n - 1);
        *lo = static_cast<int>(std::floor(std::clamp(a, 0.0f, top)));
        *hi = std::min(static_cast<int>(std::floor(std::clamp(b, 0.0f, top))) + 1, n - 1);
        return true;
    }

    const MaskDistance& md_;
    std::vector<Level> levels_;
    std::vector<std::vector<float>> owned_;  // levels 1.., which have no other home
    float slack_ = 0.0f;
};

// The stored value, given the region.
float extrude_value(float shell, float region, float round) {
    return round > 0.0f ? kernel::op_sintersect_quadratic(shell, region, round)
                        : kernel::op_intersect(shell, region);
}

// True when every region at or below `ceiling` makes extrude_value return
// `shell` exactly. cmax(shell, region) is shell once shell > region. The smooth
// form is -(cmin(-shell, -region) - h*h*s/4) with h = max(s - |shell - region|,
// 0) / s, which is exactly zero once shell - region >= s; float subtraction
// keeps that ordering because rounding is monotone.
bool shell_decides(float shell, float ceiling, float round) {
    if (!(shell > ceiling)) return false;
    return !(round > 0.0f) || shell - ceiling >= kernel::csmin_quadratic_support(round);
}

// The field extrude's per-brick fill. With `cull` off every sample is
// projected, which is the reference the culled fill must match bit for bit.
//
// The projection's gradient is a central difference over the LATTICE: the
// distances one cell either side, which the fill has already taken for every
// neighbour in the window it was handed. A lattice point is the same float
// position whichever brick names it (BrickGrid::sample_position adds integers
// before converting), so a neighbour across a brick face is read from the
// brick that sampled it, and only one outside the window costs a source call
// at that same position. Either way a sample two bricks share gets the same
// gradient in each, and the halo stays seamless. #667's six off-lattice taps
// cost six calls on every sample.
class ExtrudeFill {
  public:
    ExtrudeFill(const std::function<float(cfloat3)>& source, const MaskDistance& md,
                const RegionCeiling& ceiling, const MaskExtrudeSettings& settings, float round,
                bool cull)
        : source_(source), md_(md), ceiling_(ceiling), side_(settings.side),
          thickness_(settings.thickness), round_(round), cull_(cull) {}

    void operator()(const field::FieldVolume::BrickGrid& grid, std::size_t first,
                    std::size_t count, float* out) {
        first_ = first;
        count_ = count;
        distance_.resize(count * field::kBrickSamples);
        // The shell first, one source call a sample, for the whole window.
        for (std::size_t s = 0; s < count; ++s)
            for (int i = 0; i < field::kBrickSamples; ++i) {
                const std::size_t at = s * field::kBrickSamples + static_cast<std::size_t>(i);
                distance_[at] = source_(grid.sample_position(first + s, i));
                out[at] = shell_of(distance_[at], side_, thickness_);
            }
        for (std::size_t s = 0; s < count; ++s)
            project_brick(grid, s, out + s * field::kBrickSamples);
        tally_.samples += count * field::kBrickSamples;
    }

    const detail::MaskExtrudeTally& tally() const { return tally_; }

  private:
    static constexpr int kSide = field::kBrickDim + 1;
    static constexpr int kStride[3] = {1, kSide, kSide * kSide};

    void project_brick(const field::FieldVolume::BrickGrid& grid, std::size_t s, float* block) {
        // A brick whose every shell lies beyond the band is dropped by the
        // volume whatever the region says, because the stored value is never
        // below the shell, and all that survives of it is a sign the shell
        // already has.
        if (cull_ && std::all_of(block, block + field::kBrickSamples,
                                 [&grid](float shell) { return shell > grid.band; })) {
            ++tally_.bricks_beyond_band;
            return;
        }
        const float* distance = distance_.data() + s * field::kBrickSamples;
        for (int i = 0; i < field::kBrickSamples; ++i) {
            const cfloat3 p = grid.sample_position(first_ + s, i);
            if (cull_ && shell_decides(block[i], ceiling_.above(p, std::abs(distance[i])), round_)) {
                ++tally_.bound_skips;
                if (std::abs(block[i]) <= grid.band) ++tally_.bound_skips_in_band;
                continue;
            }
            ++tally_.projected;
            const cfloat3 q = project_to_surface(p, gradient(grid, s, i), distance[i]);
            block[i] = extrude_value(block[i], md_.eval(q), round_);
        }
    }

    cfloat3 gradient(const field::FieldVolume::BrickGrid& grid, std::size_t s, int i) const {
        float g[3];
        for (int axis = 0; axis < 3; ++axis)
            g[axis] = neighbour(grid, s, i, axis, +1) - neighbour(grid, s, i, axis, -1);
        return cf3(g[0], g[1], g[2]);
    }

    // The source's distance at sample `i` of window brick `s`, stepped one cell
    // along `axis`.
    float neighbour(const field::FieldVolume::BrickGrid& grid, std::size_t s, int i, int axis,
                    int step) const {
        const int local[3] = {i % kSide, (i / kSide) % kSide, i / (kSide * kSide)};
        const int moved = local[axis] + step;
        if (moved >= 0 && moved < kSide)
            return distance_[s * field::kBrickSamples +
                             static_cast<std::size_t>(i + step * kStride[axis])];
        // Across the face: the neighbouring brick holds this lattice point one
        // cell in from its own opposite face.
        const std::size_t slot = first_ + s;
        const std::int32_t brick[3] = {
            static_cast<std::int32_t>(slot % static_cast<std::size_t>(grid.bcount[0])),
            static_cast<std::int32_t>((slot / static_cast<std::size_t>(grid.bcount[0])) %
                                      static_cast<std::size_t>(grid.bcount[1])),
            static_cast<std::int32_t>(slot / (static_cast<std::size_t>(grid.bcount[0]) *
                                              static_cast<std::size_t>(grid.bcount[1])))};
        const std::int64_t slot_stride[3] = {
            1, grid.bcount[0], static_cast<std::int64_t>(grid.bcount[0]) * grid.bcount[1]};
        const std::int64_t next = static_cast<std::int64_t>(slot) + step * slot_stride[axis];
        const bool in_lattice = brick[axis] + step >= 0 && brick[axis] + step < grid.bcount[axis];
        if (in_lattice && next >= static_cast<std::int64_t>(first_) &&
            next < static_cast<std::int64_t>(first_ + count_)) {
            const int there = step > 0 ? 1 : field::kBrickDim - 1;
            const std::size_t brick_at = static_cast<std::size_t>(next) - first_;
            return distance_[brick_at * field::kBrickSamples +
                             static_cast<std::size_t>(i + (there - local[axis]) * kStride[axis])];
        }
        // Outside the window: the source, at the position the neighbouring
        // brick's own fill is handed, through the one function that makes it.
        int g[3];
        grid.sample_cell(slot, i, g);
        g[axis] += step;
        return source_(grid.cell_position(g));
    }

    const std::function<float(cfloat3)>& source_;
    const MaskDistance& md_;
    const RegionCeiling& ceiling_;
    ExtrudeSide side_;
    float thickness_;
    float round_;
    bool cull_;
    std::size_t first_ = 0, count_ = 0;
    std::vector<float> distance_;  // the window's source distances, brick-major
    detail::MaskExtrudeTally tally_;
};

}  // namespace

// -- the public conversion ----------------------------------------------------

std::optional<field::FieldVolume> mask_to_field(const voxel::MaskField& mask, float threshold, float band,
                                         float pad, float cell_size) {
    const float cell = cell_size > 0.0f ? cell_size : mask.cell_size();
    const float use_band = band > 0.0f ? band : cell * 3.0f;
    std::optional<MaskDistance> md = measure(mask, threshold, pad + use_band, cell);
    if (!md) return std::nullopt;
    field::FieldVolume out =
        field::FieldVolume::sample([&md](cfloat3 p) { return md->eval(p); }, md->bounds(), cell,
                            use_band);
    return out;
}

// -- the extrude, on a field --------------------------------------------------

namespace {

std::optional<field::FieldVolume> extrude_field(const std::function<float(cfloat3)>& source,
                                                const voxel::MaskField& mask,
                                                const MaskExtrudeSettings& settings,
                                                parallel::CancelToken* token, bool cull,
                                                detail::MaskExtrudeTally* tally) {
    if (!(settings.thickness > 0.0f) || mask.empty() || !source) return std::nullopt;
    const float cell = settings.cell_size > 0.0f ? settings.cell_size : mask.cell_size();
    if (!(cell > 0.0f)) return std::nullopt;
    // A wall thinner than a cell has no samples inside it, so what came back
    // would be an empty volume rather than a thin one.
    if (settings.thickness < cell) return std::nullopt;
    const float band = settings.band > 0.0f ? settings.band : cell * 3.0f;
    const float round = std::max(settings.border_round, 0.0f);

    // Border smoothing acts on a COPY: a verb that produced geometry and also
    // rewrote its own input would make "extract twice at two thicknesses" mean
    // two different borders.
    voxel::MaskField shaped = mask;
    if (settings.border_smooth > 0) shaped.smooth(settings.border_smooth);

    // Pad past the mask by everything that reaches outside it. Clipping the
    // measurement at the mask's own border would put a wall there.
    std::optional<MaskDistance> md =
        measure(shaped, settings.threshold, settings.thickness + round + band + 2.0f * cell, cell);
    if (!md) return std::nullopt;

    // The same positions, windows and cancel checkpoints FieldVolume::sample
    // would use; only the fill is the extrude's own, so it can see a window of
    // bricks at a time. A culled brick holds shells rather than exact values,
    // but every one of them is above the band and so above zero, which is all
    // `deepest` is asked about.
    const RegionCeiling ceiling(*md);
    ExtrudeFill fill(source, *md, ceiling, settings, round, cull);
    float deepest = kInf;
    bool cancelled = false;
    field::FieldVolume out = field::FieldVolume::sample_blocks(
        [&](const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
            float* values) {
            fill(grid, first, count, values);
            for (std::size_t i = 0; i < count * field::kBrickSamples; ++i)
                deepest = std::min(deepest, values[i]);
        },
        md->bounds(), cell, band, token, &cancelled);
    if (tally) *tally = fill.tally();

    // A cancel and "the mask never reached the surface" both return nullopt,
    // and a host must not be shown the second when the user did the first —
    // the token is how they are told apart, so it is checked before the
    // geometric refusal below.
    if (cancelled) return std::nullopt;

    // No sample landed inside: the masked region never reached the source's
    // surface. That is the common mistake, and an empty volume handed back would
    // read as a bug in the caller's painting rather than in their aim.
    if (!(deepest < 0.0f)) return std::nullopt;

    // Measured rather than bounded in advance, for flatten's reason: a rounded
    // intersection of two fields is a bound, and an evaluator told it was exact
    // oversteps.
    out.set_sample_lipschitz(out.measure_sample_lipschitz());
    return out;
}

}  // namespace

std::optional<field::FieldVolume> mask_extrude(const std::function<float(cfloat3)>& source,
                                               const voxel::MaskField& mask,
                                               const MaskExtrudeSettings& settings,
                                               parallel::CancelToken* token) {
    return extrude_field(source, mask, settings, token, true, nullptr);
}

namespace detail {

std::optional<field::FieldVolume> mask_extrude_field(
    const std::function<float(cfloat3)>& source, const voxel::MaskField& mask,
    const MaskExtrudeSettings& settings, bool cull, MaskExtrudeTally* tally) {
    return extrude_field(source, mask, settings, nullptr, cull, tally);
}

}  // namespace detail

// -- the extrude, on voxels ---------------------------------------------------

std::optional<voxel::VoxelGrid> mask_extrude(const voxel::VoxelGrid& grid,
                                             const voxel::MaskField& mask,
                                             const MaskExtrudeSettings& settings,
                                             parallel::CancelToken* token) {
    if (!(settings.thickness > 0.0f) || mask.empty() || grid.occupied_count() == 0)
        return std::nullopt;
    const float vs = grid.voxel_size();

    voxel::MaskField shaped = mask;
    if (settings.border_smooth > 0) shaped.smooth(settings.border_smooth);

    std::optional<VoxelCoord> mlo = shaped.bounds_min(), mhi = shaped.bounds_max();
    if (!mlo || !mhi) return std::nullopt;
    // The mask bounds where surface seeds can be found. The grown wall may
    // extend beyond these bounds; its height is set by thickness alone.
    const float ms = shaped.cell_size();
    const auto to_grid = [vs](float world) {
        return static_cast<std::int32_t>(std::floor(world / vs));
    };
    const VoxelCoord lo{to_grid(static_cast<float>(mlo->x) * ms) - 1,
                        to_grid(static_cast<float>(mlo->y) * ms) - 1,
                        to_grid(static_cast<float>(mlo->z) * ms) - 1};
    const VoxelCoord hi{to_grid(static_cast<float>(mhi->x + 1) * ms) + 1,
                        to_grid(static_cast<float>(mhi->y + 1) * ms) + 1,
                        to_grid(static_cast<float>(mhi->z + 1) * ms) + 1};

    const auto centre = [vs](VoxelCoord c) {
        return cf3(static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f,
                   static_cast<float>(c.z) + 0.5f) *
               vs;
    };
    const auto masked = [&](VoxelCoord c) {
        return shaped.sample(centre(c)) >= settings.threshold;
    };

    int out_layers = 0, in_layers = 0;
    switch (settings.side) {
        case ExtrudeSide::Outward:
            out_layers = layers_for(settings.thickness, vs);
            break;
        case ExtrudeSide::Inward:
            in_layers = layers_for(settings.thickness, vs);
            break;
        case ExtrudeSide::Centred:
            out_layers = layers_for(settings.thickness * 0.5f, vs);
            in_layers = out_layers;
            break;
    }

    static const std::array<VoxelCoord, 6> kFaces = {VoxelCoord{1, 0, 0},  VoxelCoord{-1, 0, 0},
                                                     VoxelCoord{0, 1, 0},  VoxelCoord{0, -1, 0},
                                                     VoxelCoord{0, 0, 1},  VoxelCoord{0, 0, -1}};
    const auto step = [](VoxelCoord c, VoxelCoord d) {
        return VoxelCoord{c.x + d.x, c.y + d.y, c.z + d.z};
    };

    // Seeds: masked, occupied, and on the surface. A cell buried in the interior
    // is not where an extract starts, whichever side it grows toward.
    std::vector<std::pair<VoxelCoord, std::uint8_t>> seeds;
    for (std::int32_t z = lo.z; z <= hi.z; ++z) {
        // The checkpoint is the outer slice: one relaxed load per z-plane
        // rather than one per cell. A cancelled extract returns nullopt and the
        // caller installs nothing, so nothing partial escapes.
        if (parallel::cancelled(token)) return std::nullopt;
        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) {
                const VoxelCoord c{x, y, z};
                const std::uint8_t idx = grid.get(c);
                if (idx == 0 || !masked(c)) continue;
                bool surface = false;
                for (VoxelCoord f : kFaces) surface = surface || grid.get(step(c, f)) == 0;
                if (surface) seeds.emplace_back(c, idx);
            }
    }
    if (seeds.empty()) return std::nullopt;

    voxel::VoxelGrid out(vs);
    std::array<std::uint8_t, 256> remap{};
    const auto colour_of = [&](std::uint8_t src) {
        if (remap[src] == 0) remap[src] = out.palette_add(grid.palette_color(src));
        return remap[src];
    };

    // Follow each seed's surface normal rather than flooding through adjacent
    // mask cells. Flooding spreads sideways with each layer, so a thicker wall
    // widens its footprint and diverges from the field extract.
    for (const auto& [seed, idx] : seeds) {
        if (parallel::cancelled(token)) return std::nullopt;
        cfloat3 normal = cf3(0.0f, 0.0f, 0.0f);
        for (int dz = -2; dz <= 2; ++dz)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const VoxelCoord near{seed.x + dx, seed.y + dy, seed.z + dz};
                    if (grid.get(near) != 0) continue;
                    normal = normal + cf3(static_cast<float>(dx), static_cast<float>(dy),
                                           static_cast<float>(dz));
                }
        const float length = kernel::clength(normal);
        if (length == 0.0f) continue;
        normal = normal / length;
        const cfloat3 origin = centre(seed);
        const auto cell_at = [&](float distance) {
            const cfloat3 p = origin + normal * distance;
            return VoxelCoord{to_grid(p.x), to_grid(p.y), to_grid(p.z)};
        };

        // Inward includes the surface seed; outward begins in empty space.
        for (int layer = 0; layer < in_layers; ++layer) {
            const VoxelCoord cell = cell_at(-static_cast<float>(layer) * vs);
            const std::uint8_t here = grid.get(cell);
            if (here != 0 && out.get(cell) == 0) out.set(cell, colour_of(here));
        }
        for (int layer = 1; layer <= out_layers; ++layer) {
            const VoxelCoord cell = cell_at(static_cast<float>(layer) * vs);
            if (grid.get(cell) == 0 && out.get(cell) == 0) out.set(cell, colour_of(idx));
        }
    }

    if (out.occupied_count() == 0) return std::nullopt;
    return out;
}

}  // namespace brush
}  // namespace clay
