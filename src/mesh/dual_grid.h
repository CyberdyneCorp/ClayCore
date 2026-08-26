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
#include "clay/mesh/marching.h"  // MeshingOptions + apply_tape_attributes
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

// Gibson surface nets vertex: centroid of the cell's edge crossings (always
// inside the cell — it is a convex combination of points on the cell). Here
// rather than in surface_nets.cpp because the quad mesher places its vertices
// with it too, and two copies of this would be two surfaces.
inline kernel::cfloat3 nets_vertex(const DualCrossing* crossings, int count, kernel::cfloat3 lo,
                                   kernel::cfloat3 hi) {
    if (count == 0) return (lo + hi) * 0.5f;  // unreachable for sign-changing cells
    kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
    for (int i = 0; i < count; ++i) sum = sum + crossings[i].pos;
    return sum / static_cast<float>(count);
}

// Core dual-grid mesher. Cells [cell_min, cell_max) are visited; a lattice
// edge produces a quad only when all four adjacent cells own vertices, so
// callers wanting closed output must pad the range by one ring of positive
// samples (as the tape-level wrappers do).
//
// `keep_quads` additionally records the four corners in Mesh::quads. It
// changes nothing else — same vertices, same triangles, same 0-2 diagonal —
// so the quad meshers and the triangle meshers are one code path and cannot
// drift into two that disagree about where the surface is.
// TEMPLATED ON THE SAMPLER, not taking a std::function, and that is the whole
// of why this is fast enough to be a preview. `sample` is called for eight
// lattice corners of every cell in the range -- 33 million calls for a 0.02
// mesh of the benchmark document -- and through a std::function none of them
// inlines. Measured on that lattice: 52.6 ms of indirect calls against 6.7 ms
// for the same reads inlined. The tape-level meshers below pass a lambda and
// get that; `mesh_lattice_nets` and `mesh_lattice_dc` keep their public
// std::function parameter and instantiate this on it, which is no worse than
// what they paid before.
template <typename Sample>
inline Mesh dual_grid_mesh(const Sample& sample, const int cell_min[3], const int cell_max[3],
                           kernel::cfloat3 origin, float spacing, const DualNormalFn& normal_at,
                           const DualPlacer& place, bool keep_quads = false) {
    using kernel::cf3;
    using kernel::cfloat3;
    // corner bit c: x=1, y=2, z=4 (same convention as the marching mesher)
    constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                   {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    // Cell (i,j,k) owns the three lattice edges leaving its min corner. For edge
    // axis d, (d,u,v) below is a right-handed permutation, so the loop
    // [c11, c01, c00, c10] of adjacent cells has normal +d.
    constexpr int kAxisUV[3][2] = {{1, 2}, {2, 0}, {0, 1}};
    Mesh m;
    std::unordered_map<std::uint64_t, std::uint32_t> cell_vertex;

    // ONE walk, placing a cell's vertex and then emitting the quads on the
    // edges it owns, where this used to walk the whole range twice.
    //
    // The quads can be emitted here because every cell a quad references is at
    // or before this one in (k, j, i) order: `cell_at` moves back along the two
    // axes that are not the edge's, never forward, so the four cells around an
    // owned edge are this one and three already placed. The vertices and the
    // indices therefore come out in exactly the order the two-pass form
    // produced them.
    //
    // What it saves is a whole pass of SAMPLING. The second pass read four
    // corners of every cell in the range to find its sign-changing edges, and
    // all four -- the min corner and its three axis neighbours -- are corners
    // 0, 1, 2 and 4 of the eight this pass already has. It also read them for
    // cells that own no vertex, which cannot contribute a quad at all: an edge
    // leaving the min corner changes sign only if two of the cell's corners
    // differ, and then the cell is a surface cell. Measured on a 0.02 mesh of
    // the benchmark document, that second pass was 27.4 ms of sampling for
    // nothing.
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

                // The three edges leaving the min corner: corner 0 against
                // corners 1, 2 and 4, which are f[1 << d].
                const float f0 = f[0];
                for (int d = 0; d < 3; ++d) {
                    const float f1 = f[1 << d];
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
                    if (keep_quads) m.quads.insert(m.quads.end(), quad, quad + 4);
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

// The tape-level surface-nets mesh, shared by mesh_tape_nets and
// mesh_tape_quads so the two cannot drift apart: one lattice, one sampler, one
// closing ring of out-of-range positive samples, one attribute pass.
// `keep_quads` is the ONLY difference between the two calls, which is what
// makes "the quad mesh is the nets mesh plus its quads" a property of the code
// rather than of a test.
inline Mesh tape_nets_mesh(const scene::Tape& tape, const math::Aabb& region, float cell_size,
                           const MeshingOptions& options, bool keep_quads) {
    TapeGrid grid = eval_tape_grid(tape, region, cell_size);
    if (!grid.ok) return {};
    auto sample = [&](int i, int j, int k) { return grid.at(i, j, k); };
    // one extra cell ring: out-of-range samples are positive, so geometry
    // crossing the region boundary is closed (same ring as mesh_tape)
    int cmin[3] = {-1, -1, -1};
    int cmax[3] = {grid.nx, grid.ny, grid.nz};
    Mesh m = dual_grid_mesh(sample, cmin, cmax, region.min, cell_size, {}, nets_vertex, keep_quads);
    apply_tape_attributes(m, tape, options);
    return m;
}

}  // namespace detail
}  // namespace mesh
}  // namespace clay
