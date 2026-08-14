#include "clay/mesh/quad_mesh.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "dual_grid.h"

namespace clay {
namespace mesh {

namespace {

// How many lattice points a region needs at a cell size — the quantity
// mesh_tape_quads prices against kMaxGridSamples. In double throughout: the
// product overflows a 32-bit count long before it reaches the ceiling, which
// is the failure the ceiling exists to prevent.
double lattice_samples(const math::Aabb& region, double cell) {
    const double dx = static_cast<double>(region.max.x - region.min.x) / cell + 2.0;
    const double dy = static_cast<double>(region.max.y - region.min.y) / cell + 2.0;
    const double dz = static_cast<double>(region.max.z - region.min.z) / cell + 2.0;
    return dx * dy * dz;
}

// The finest cell size the mesher would still accept for this region. Found by
// bisection rather than by solving the cubic: the count is monotone in the
// cell size, forty halvings pin it to a part in 10^12, and an inverted cubic
// would have to be checked against the pricing anyway to be trusted.
float pricing_floor(const math::Aabb& region, float coarse) {
    double lo = static_cast<double>(coarse) * 1e-9;  // certainly refused
    double hi = static_cast<double>(coarse);         // certainly affordable
    if (!(lattice_samples(region, hi) <= static_cast<double>(kMaxGridSamples))) return coarse;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (lattice_samples(region, mid) <= static_cast<double>(kMaxGridSamples))
            hi = mid;
        else
            lo = mid;
    }
    return static_cast<float>(hi);
}

// Is `count` a better answer to `want` than `best` is? Never-empty beats
// empty first, so a target that collapses the shape's topology comes back as
// the best REAL mesh rather than as the collapse; then plain distance; then
// the tie-break the contract names — the candidate that does not exceed the
// target wins, because "about this many" is usually read as "no more than".
bool better_count(std::size_t count, std::size_t best, double want) {
    if ((count == 0) != (best == 0)) return best == 0;
    const double d = std::fabs(static_cast<double>(count) - want);
    const double db = std::fabs(static_cast<double>(best) - want);
    if (d != db) return d < db;
    return static_cast<double>(count) <= want && static_cast<double>(best) > want;
}

}  // namespace

QuadFit fit_quad_cell(const std::function<Mesh(float)>& mesh_at, float seed_cell, float min_cell,
                      float max_cell, const QuadTarget& target, Mesh* out_mesh) {
    QuadFit fit;
    if (!mesh_at) return fit;
    if (!(min_cell > 0.0f) || !std::isfinite(min_cell)) return fit;
    if (!(max_cell >= min_cell) || !std::isfinite(max_cell)) return fit;

    bool clamped = false;
    auto clamp_cell = [&](float c) {
        if (!std::isfinite(c) || c < min_cell) {
            clamped = true;
            return min_cell;
        }
        if (c > max_cell) {
            clamped = true;
            return max_cell;
        }
        return c;
    };

    float cell = clamp_cell(seed_cell);
    if (target.target == 0) {
        // No search ran, so no iteration is charged and there is no tolerance
        // to have missed. The report is here to echo the cell size the mesh
        // was actually built at, which a caller who passed 0 does not know.
        Mesh m = mesh_at(cell);
        fit.cell_size = cell;
        fit.quad_count = m.quad_count();
        fit.within_tolerance = true;
        fit.clamped = clamped;
        if (out_mesh) *out_mesh = std::move(m);
        return fit;
    }

    const double want = static_cast<double>(target.target);
    const float tolerance = target.tolerance > 0.0f ? target.tolerance : 0.10f;
    const int cap = target.max_iterations > 0 ? target.max_iterations : 4;

    Mesh best;
    bool have_best = false;
    for (int i = 0; i < cap; ++i) {
        Mesh m = mesh_at(cell);
        const std::size_t count = m.quad_count();
        ++fit.iterations;
        if (!have_best || better_count(count, fit.quad_count, want)) {
            best = std::move(m);
            fit.cell_size = cell;
            fit.quad_count = count;
            have_best = true;
        }
        if (count > 0 &&
            std::fabs(static_cast<double>(count) - want) <= static_cast<double>(tolerance) * want)
            break;

        // A lattice that found NOTHING carries no information about how far
        // off it is — sqrt(0 / want) is zero, and the shape is simply not
        // resolved yet — so the step is a plain refinement instead.
        double ratio = 0.5;
        if (count > 0)
            ratio = std::clamp(std::sqrt(static_cast<double>(count) / want), 0.5, 2.0);
        const float next = clamp_cell(static_cast<float>(static_cast<double>(cell) * ratio));
        // The next lattice is the one just meshed: either the correction was
        // below the float's resolution or a limit is where we already stand,
        // and another iteration would rebuild the same mesh.
        if (next == cell) break;
        cell = next;
    }

    fit.within_tolerance =
        fit.quad_count > 0 && std::fabs(static_cast<double>(fit.quad_count) - want) <=
                                  static_cast<double>(tolerance) * want;
    fit.clamped = clamped;
    if (out_mesh) *out_mesh = std::move(best);
    return fit;
}

Mesh mesh_tape_quads_fit(const scene::Tape& tape, const math::Aabb& region, float cell_size,
                         const QuadTarget& target, const MeshingOptions& options,
                         QuadFit* out_fit) {
    Mesh mesh;
    QuadFit fit;
    if (region.empty() || region.is_infinite()) {
        if (out_fit) *out_fit = fit;
        return mesh;
    }
    const kernel::cfloat3 extent = region.extent();
    const float longest = kernel::cmax(extent.x, kernel::cmax(extent.y, extent.z));
    if (!(longest > 0.0f) || !std::isfinite(longest)) {
        if (out_fit) *out_fit = fit;
        return mesh;
    }

    // The seed. A caller's cell size is taken as given; otherwise the count is
    // area / cell^2 and the only area known before a mesh exists is the
    // region's own box, which overestimates a blobby shape's and so seeds
    // slightly coarse. It is a starting point for the secant, not a claim.
    float seed = cell_size;
    if (!(seed > 0.0f) || !std::isfinite(seed)) {
        if (target.target == 0) {
            if (out_fit) *out_fit = fit;
            return mesh;  // no cell size and no target names no lattice at all
        }
        const double area = 2.0 * (static_cast<double>(extent.x) * extent.y +
                                   static_cast<double>(extent.y) * extent.z +
                                   static_cast<double>(extent.z) * extent.x);
        seed = static_cast<float>(std::sqrt(area / static_cast<double>(target.target)));
    }

    // A caller's floor above the coarse limit would leave an empty range, so
    // it is capped there: the answer to "no finer than the whole region" is
    // the whole region, reported as clamped, not a refusal with no mesh.
    const float asked_floor = (std::isfinite(target.min_cell_size) && target.min_cell_size > 0.0f)
                                  ? target.min_cell_size
                                  : 0.0f;
    const float floor_cell =
        std::min(std::max(pricing_floor(region, longest), asked_floor), longest);
    fit = fit_quad_cell(
        [&](float cell) { return mesh_tape_quads(tape, region, cell, options); }, seed,
        floor_cell, longest, target, &mesh);
    if (out_fit) *out_fit = fit;
    return mesh;
}

Mesh mesh_lattice_quads(const std::function<float(int, int, int)>& sample, const int cell_min[3],
                        const int cell_max[3], kernel::cfloat3 origin, float spacing) {
    return detail::dual_grid_mesh(sample, cell_min, cell_max, origin, spacing, {},
                                  detail::nets_vertex, /*keep_quads=*/true);
}

Mesh mesh_tape_quads(const scene::Tape& tape, const math::Aabb& region, float cell_size,
                     const MeshingOptions& options) {
    if (region.empty() || region.is_infinite()) return {};
    // The same pricing mesh_tape applies, and for the same reason: the lattice
    // is sized from the caller's cell size, so an over-fine number ends the
    // process in the allocator rather than returning. Priced in double before
    // anything is cast to int. It is also what a count search needs — a
    // ceiling it can stop at and report, rather than an allocation it cannot
    // survive to report.
    if (!(cell_size > 0.0f) || !std::isfinite(cell_size)) return {};
    const double dx = static_cast<double>(region.max.x - region.min.x) / cell_size + 2.0;
    const double dy = static_cast<double>(region.max.y - region.min.y) / cell_size + 2.0;
    const double dz = static_cast<double>(region.max.z - region.min.z) / cell_size + 2.0;
    if (!(dx * dy * dz <= static_cast<double>(kMaxGridSamples))) return {};

    return detail::tape_nets_mesh(tape, region, cell_size, options, /*keep_quads=*/true);
}

bool quads_consistent(const Mesh& m) {
    if (m.quads.empty()) return true;
    if (m.quads.size() % 4 != 0) return false;
    if (m.indices.size() != m.quad_count() * 6) return false;
    const std::uint32_t vertices = static_cast<std::uint32_t>(m.positions.size());
    for (std::size_t q = 0; q < m.quad_count(); ++q) {
        const std::uint32_t* c = &m.quads[q * 4];
        for (int i = 0; i < 4; ++i)
            if (c[i] >= vertices) return false;
        const std::uint32_t* t = &m.indices[q * 6];
        // (a,b,c),(a,c,d) — the expansion mesh_data.h names, in order. Compared
        // index for index rather than as a set: a quad whose triangles are the
        // other diagonal's is a different surface where the quad is not planar,
        // and every quad this library makes may be non-planar.
        if (t[0] != c[0] || t[1] != c[1] || t[2] != c[2]) return false;
        if (t[3] != c[0] || t[4] != c[2] || t[5] != c[3]) return false;
    }
    return true;
}

void drop_quads(Mesh& m) { m.quads.clear(); }

}  // namespace mesh
}  // namespace clay
