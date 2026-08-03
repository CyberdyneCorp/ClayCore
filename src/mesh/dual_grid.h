#pragma once

// Internal machinery shared by the dual meshers (surface nets, dual
// contouring): one vertex per sign-changing cell — placement supplied by the
// caller — and one quad (two triangles) per sign-changing lattice edge,
// connecting the four adjacent cell vertices, wound toward positive field.
// Not part of the public API.

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/kernel/shim.h"
#include "clay/math/geom.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace mesh {
namespace detail {

struct DualCrossing {
    kernel::cfloat3 pos;     // world-space crossing on a cell edge
    kernel::cfloat3 normal;  // unit field gradient there; zero when unused
};

// Places the cell vertex from its edge crossings; [lo, hi] is the world cell box.
using DualPlacer = std::function<kernel::cfloat3(const DualCrossing* crossings, int count,
                                                 kernel::cfloat3 lo, kernel::cfloat3 hi)>;

// Hermite normal source: unit gradient at world point p. The lattice edge is
// identified by its endpoints p0/p1 and interpolation t for samplers that
// differentiate on the lattice instead of in world space.
using DualNormalFn = std::function<kernel::cfloat3(kernel::cfloat3 p, const int p0[3],
                                                   const int p1[3], float t)>;

inline std::uint64_t pack_cell(int i, int j, int k) {
    constexpr std::uint64_t bias = 1u << 20;
    return ((static_cast<std::uint64_t>(i) + bias) << 42) |
           ((static_cast<std::uint64_t>(j) + bias) << 21) |
           (static_cast<std::uint64_t>(k) + bias);
}

// Dense tape sampling shared by the tape-level dual meshers; mirrors
// mesh_tape's grid setup so all meshers agree on lattice geometry.
struct TapeGrid {
    int nx = 0, ny = 0, nz = 0;
    float outside = 0.0f;  // positive constant returned beyond the region
    std::vector<float> values;
    bool ok = false;

    float at(int i, int j, int k) const {
        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return outside;
        return values[(static_cast<std::size_t>(k) * ny + j) * nx + i];
    }
};

inline TapeGrid eval_tape_grid(const scene::Tape& tape, const math::Aabb& region,
                               float voxel_size) {
    TapeGrid g;
    if (region.empty() || region.is_infinite()) return g;
    g.nx = static_cast<int>(kernel::cround((region.max.x - region.min.x) / voxel_size)) + 1;
    g.ny = static_cast<int>(kernel::cround((region.max.y - region.min.y) / voxel_size)) + 1;
    g.nz = static_cast<int>(kernel::cround((region.max.z - region.min.z) / voxel_size)) + 1;
    g.outside = kernel::cabs(voxel_size);

    eval::GridQuery grid;
    grid.origin = region.min;
    grid.spacing = voxel_size;
    grid.nx = g.nx;
    grid.ny = g.ny;
    grid.nz = g.nz;
    g.values.resize(static_cast<std::size_t>(g.nx) * g.ny * g.nz);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    g.ok = cpu && cpu->eval_grid(tape, grid, g.values.data()) == eval::Status::Ok;
    return g;
}

// Core dual-grid mesher. Cells [cell_min, cell_max) are visited; a lattice
// edge produces a quad only when all four adjacent cells own vertices, so
// callers wanting closed output must pad the range by one ring of positive
// samples (as the tape-level wrappers do).
inline Mesh dual_grid_mesh(const std::function<float(int, int, int)>& sample,
                           const int cell_min[3], const int cell_max[3], kernel::cfloat3 origin,
                           float spacing, const DualNormalFn& normal_at,
                           const DualPlacer& place) {
    using kernel::cf3;
    using kernel::cfloat3;
    // corner bit c: x=1, y=2, z=4 (same convention as the marching mesher)
    constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                   {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    Mesh m;
    std::unordered_map<std::uint64_t, std::uint32_t> cell_vertex;

    for (int k = cell_min[2]; k < cell_max[2]; ++k)
        for (int j = cell_min[1]; j < cell_max[1]; ++j)
            for (int i = cell_min[0]; i < cell_max[0]; ++i) {
                float f[8];
                bool any_neg = false, any_pos = false;
                for (int c = 0; c < 8; ++c) {
                    f[c] = sample(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                    (f[c] < 0.0f ? any_neg : any_pos) = true;
                }
                if (!any_neg || !any_pos) continue;
                DualCrossing crossings[12];
                int n = 0;
                for (const auto& e : kEdges) {
                    float f0 = f[e[0]], f1 = f[e[1]];
                    if ((f0 < 0.0f) == (f1 < 0.0f)) continue;
                    int p0[3] = {i + (e[0] & 1), j + ((e[0] >> 1) & 1), k + ((e[0] >> 2) & 1)};
                    int p1[3] = {i + (e[1] & 1), j + ((e[1] >> 1) & 1), k + ((e[1] >> 2) & 1)};
                    float t = f0 / (f0 - f1);
                    cfloat3 a = origin + cf3((float)p0[0], (float)p0[1], (float)p0[2]) * spacing;
                    cfloat3 b = origin + cf3((float)p1[0], (float)p1[1], (float)p1[2]) * spacing;
                    crossings[n].pos = a + (b - a) * t;
                    crossings[n].normal =
                        normal_at ? normal_at(crossings[n].pos, p0, p1, t) : cf3(0, 0, 0);
                    ++n;
                }
                cfloat3 lo = origin + cf3((float)i, (float)j, (float)k) * spacing;
                cfloat3 hi = lo + cf3(1, 1, 1) * spacing;
                cell_vertex.emplace(pack_cell(i, j, k),
                                    static_cast<std::uint32_t>(m.positions.size()));
                m.positions.push_back(place(crossings, n, lo, hi));
            }

    // Quad pass: cell (i,j,k) owns the three lattice edges leaving its min
    // corner. For edge axis d, (d,u,v) below is a right-handed permutation,
    // so the loop [c11, c01, c00, c10] of adjacent cells has normal +d.
    constexpr int kAxisUV[3][2] = {{1, 2}, {2, 0}, {0, 1}};
    for (int k = cell_min[2]; k < cell_max[2]; ++k)
        for (int j = cell_min[1]; j < cell_max[1]; ++j)
            for (int i = cell_min[0]; i < cell_max[0]; ++i) {
                float f0 = sample(i, j, k);
                for (int d = 0; d < 3; ++d) {
                    int q[3] = {i, j, k};
                    q[d] += 1;
                    float f1 = sample(q[0], q[1], q[2]);
                    if ((f0 < 0.0f) == (f1 < 0.0f)) continue;
                    const int u = kAxisUV[d][0], v = kAxisUV[d][1];
                    auto cell_at = [&](int a, int b) -> const std::uint32_t* {
                        int c[3] = {i, j, k};
                        c[u] -= a;
                        c[v] -= b;
                        auto it = cell_vertex.find(pack_cell(c[0], c[1], c[2]));
                        return it == cell_vertex.end() ? nullptr : &it->second;
                    };
                    const std::uint32_t* c11 = cell_at(1, 1);
                    const std::uint32_t* c01 = cell_at(0, 1);
                    const std::uint32_t* c00 = cell_at(0, 0);
                    const std::uint32_t* c10 = cell_at(1, 0);
                    if (!c11 || !c01 || !c00 || !c10) continue;  // open lattice boundary
                    std::uint32_t quad[4] = {*c11, *c01, *c00, *c10};
                    if (f0 >= 0.0f) std::swap(quad[1], quad[3]);  // wind toward positive field
                    m.indices.push_back(quad[0]);
                    m.indices.push_back(quad[1]);
                    m.indices.push_back(quad[2]);
                    m.indices.push_back(quad[0]);
                    m.indices.push_back(quad[2]);
                    m.indices.push_back(quad[3]);
                }
            }
    return m;
}

}  // namespace detail
}  // namespace mesh
}  // namespace clay
