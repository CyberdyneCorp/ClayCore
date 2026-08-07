// Mask extrude (sdf-kernels and voxel-engine specs, add-mask-extrude). See
// include/clay/field/mask_extrude.h for why the only new mechanism here is
// measuring a mask as a distance, and why the mask needs no region parameter.

#include "clay/brush/mask_extrude.h"

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

std::optional<field::FieldVolume> mask_extrude(const std::function<float(cfloat3)>& source,
                                        const voxel::MaskField& mask,
                                        const MaskExtrudeSettings& settings) {
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

    float deepest = kInf;
    field::FieldVolume out = field::FieldVolume::sample(
        [&](cfloat3 p) {
            const float shell = shell_of(source(p), settings.side, settings.thickness);
            const float region = md->eval(p);
            const float v = round > 0.0f ? kernel::op_sintersect_quadratic(shell, region, round)
                                         : kernel::op_intersect(shell, region);
            deepest = std::min(deepest, v);
            return v;
        },
        md->bounds(), cell, band);

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

// -- the extrude, on voxels ---------------------------------------------------

std::optional<voxel::VoxelGrid> mask_extrude(const voxel::VoxelGrid& grid,
                                             const voxel::MaskField& mask,
                                             const MaskExtrudeSettings& settings) {
    if (!(settings.thickness > 0.0f) || mask.empty() || grid.occupied_count() == 0)
        return std::nullopt;
    const float vs = grid.voxel_size();

    voxel::MaskField shaped = mask;
    if (settings.border_smooth > 0) shaped.smooth(settings.border_smooth);

    std::optional<VoxelCoord> mlo = shaped.bounds_min(), mhi = shaped.bounds_max();
    if (!mlo || !mhi) return std::nullopt;
    // Everything the extract can contain is masked, so the mask's own bounds
    // bound the scan — no need to walk the grid, which may be far larger.
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
    for (std::int32_t z = lo.z; z <= hi.z; ++z)
        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) {
                const VoxelCoord c{x, y, z};
                const std::uint8_t idx = grid.get(c);
                if (idx == 0 || !masked(c)) continue;
                bool surface = false;
                for (VoxelCoord f : kFaces) surface = surface || grid.get(step(c, f)) == 0;
                if (surface) seeds.emplace_back(c, idx);
            }
    if (seeds.empty()) return std::nullopt;

    voxel::VoxelGrid out(vs);
    std::array<std::uint8_t, 256> remap{};
    const auto colour_of = [&](std::uint8_t src) {
        if (remap[src] == 0) remap[src] = out.palette_add(grid.palette_color(src));
        return remap[src];
    };

    // Inward includes the seed layer: those cells are the surface, which is
    // inside the source by half a voxel, so they are the first of the -t..0 band.
    if (in_layers > 0) {
        std::vector<std::pair<VoxelCoord, std::uint8_t>> frontier;
        for (const auto& [c, idx] : seeds) {
            if (out.get(c) != 0) continue;
            out.set(c, colour_of(idx));
            frontier.emplace_back(c, idx);
        }
        for (int layer = 1; layer < in_layers; ++layer) {
            std::vector<std::pair<VoxelCoord, std::uint8_t>> next;
            for (const auto& [c, idx] : frontier)
                for (VoxelCoord f : kFaces) {
                    const VoxelCoord n = step(c, f);
                    const std::uint8_t here = grid.get(n);
                    if (here == 0 || out.get(n) != 0 || !masked(n)) continue;
                    out.set(n, colour_of(here));
                    next.emplace_back(n, here);
                }
            frontier = std::move(next);
        }
    }

    // Outward does NOT include the seeds: the plate sits ON the surface rather
    // than replacing the voxel it grew from, which is what 0 <= d <= t means on
    // the SDF side and what keeps the two representations agreeing.
    if (out_layers > 0) {
        std::vector<std::pair<VoxelCoord, std::uint8_t>> frontier = seeds;
        for (int layer = 0; layer < out_layers; ++layer) {
            std::vector<std::pair<VoxelCoord, std::uint8_t>> next;
            for (const auto& [c, idx] : frontier)
                for (VoxelCoord f : kFaces) {
                    const VoxelCoord n = step(c, f);
                    if (grid.get(n) != 0 || out.get(n) != 0 || !masked(n)) continue;
                    out.set(n, colour_of(idx));
                    next.emplace_back(n, idx);
                }
            frontier = std::move(next);
        }
    }

    if (out.occupied_count() == 0) return std::nullopt;
    return out;
}

}  // namespace brush
}  // namespace clay
