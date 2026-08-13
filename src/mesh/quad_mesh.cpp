#include "clay/mesh/quad_mesh.h"

#include <cmath>

#include "dual_grid.h"

namespace clay {
namespace mesh {

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
