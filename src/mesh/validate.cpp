#include "clay/mesh/validate.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "clay/kernel/shim.h"

namespace clay {
namespace mesh {

using kernel::cfloat3;

namespace {

struct EdgeInfo {
    int count = 0;
    int forward = 0;  // directed occurrences a->b with a < b
};

// SAT triangle-triangle penetration test: face normals, 9 edge-cross axes,
// AND the 6 in-plane axes (cross of each normal with the other triangle's
// edges) so coplanar/near-coplanar neighbors that merely touch are not
// reported. `tol` allows touching contact (validation flags penetration).
bool tri_tri_overlap(const cfloat3* a, const cfloat3* b, float tol) {
    auto axis_separates = [&](cfloat3 axis) {
        float len = kernel::clength(axis);
        if (len < 1e-12f) return false;
        axis = axis / len;
        float amin = 3.4e38f, amax = -3.4e38f, bmin = 3.4e38f, bmax = -3.4e38f;
        for (int i = 0; i < 3; ++i) {
            float pa = kernel::cdot(axis, a[i]);
            float pb = kernel::cdot(axis, b[i]);
            amin = kernel::cmin(amin, pa);
            amax = kernel::cmax(amax, pa);
            bmin = kernel::cmin(bmin, pb);
            bmax = kernel::cmax(bmax, pb);
        }
        return amax <= bmin + tol || bmax <= amin + tol;
    };
    cfloat3 na = kernel::ccross(a[1] - a[0], a[2] - a[0]);
    cfloat3 nb = kernel::ccross(b[1] - b[0], b[2] - b[0]);
    if (axis_separates(na) || axis_separates(nb)) return false;
    for (int i = 0; i < 3; ++i) {
        cfloat3 ea = a[(i + 1) % 3] - a[i];
        cfloat3 eb = b[(i + 1) % 3] - b[i];
        if (axis_separates(kernel::ccross(na, ea))) return false;
        if (axis_separates(kernel::ccross(nb, eb))) return false;
        for (int j = 0; j < 3; ++j) {
            cfloat3 eb2 = b[(j + 1) % 3] - b[j];
            if (axis_separates(kernel::ccross(ea, eb2))) return false;
        }
    }
    return true;
}

}  // namespace

ValidationReport validate(const Mesh& m, std::size_t max_intersection_pairs) {
    ValidationReport r;
    r.vertices = m.positions.size();
    r.triangles = m.triangle_count();

    std::unordered_map<std::uint64_t, EdgeInfo> edges;
    edges.reserve(r.triangles * 3);
    for (std::size_t t = 0; t < r.triangles; ++t) {
        std::uint32_t idx[3] = {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]};
        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) {
            ++r.degenerate_triangles;
            continue;
        }
        cfloat3 n = kernel::ccross(m.positions[idx[1]] - m.positions[idx[0]],
                                   m.positions[idx[2]] - m.positions[idx[0]]);
        if (kernel::clength(n) * 0.5f < 1e-12f) ++r.sliver_triangles;
        for (int e = 0; e < 3; ++e) {
            std::uint32_t a = idx[e], b = idx[(e + 1) % 3];
            std::uint64_t key = a < b
                                    ? (static_cast<std::uint64_t>(a) << 32) | b
                                    : (static_cast<std::uint64_t>(b) << 32) | a;
            EdgeInfo& info = edges[key];
            ++info.count;
            if (a < b) ++info.forward;
        }
    }

    bool oriented = true;
    for (const auto& [key, info] : edges) {
        if (info.count == 1) ++r.boundary_edges;
        if (info.count > 2) ++r.non_manifold_edges;
        if (info.count == 2 && info.forward != 1) oriented = false;
    }
    r.watertight = r.boundary_edges == 0 && r.degenerate_triangles == 0;
    r.manifold = r.non_manifold_edges == 0;
    r.oriented = r.watertight && r.manifold && oriented;
    r.euler_characteristic = static_cast<long long>(r.vertices) -
                             static_cast<long long>(edges.size()) +
                             static_cast<long long>(r.triangles - r.degenerate_triangles);

    if (max_intersection_pairs > 0 && r.triangles > 1) {
        // uniform grid over triangle centroids; test non-adjacent pairs in
        // the same bucket up to the budget
        float cell = 0.0f;
        {
            // heuristic cell: average triangle bbox diagonal
            double sum = 0;
            for (std::size_t t = 0; t < r.triangles; ++t) {
                cfloat3 p0 = m.positions[m.indices[t * 3]];
                cfloat3 p1 = m.positions[m.indices[t * 3 + 1]];
                cfloat3 p2 = m.positions[m.indices[t * 3 + 2]];
                cfloat3 lo = kernel::cmin(kernel::cmin(p0, p1), p2);
                cfloat3 hi = kernel::cmax(kernel::cmax(p0, p1), p2);
                sum += kernel::clength(hi - lo);
            }
            cell = static_cast<float>(sum / static_cast<double>(r.triangles)) + 1e-6f;
        }
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets;
        auto bucket_of = [&](cfloat3 p) {
            auto q = [&](float v) {
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(std::floor(v / cell)) + (1 << 20));
            };
            return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
        };
        for (std::size_t t = 0; t < r.triangles; ++t) {
            cfloat3 c = (m.positions[m.indices[t * 3]] + m.positions[m.indices[t * 3 + 1]] +
                         m.positions[m.indices[t * 3 + 2]]) /
                        3.0f;
            buckets[bucket_of(c)].push_back(static_cast<std::uint32_t>(t));
        }
        std::size_t tested = 0;
        for (const auto& [key, tris] : buckets) {
            for (std::size_t i = 0; i < tris.size() && tested < max_intersection_pairs; ++i)
                for (std::size_t j = i + 1; j < tris.size() && tested < max_intersection_pairs;
                     ++j) {
                    std::uint32_t ta = tris[i], tb = tris[j];
                    // skip pairs sharing any vertex (adjacent by construction)
                    bool adjacent = false;
                    for (int u = 0; u < 3 && !adjacent; ++u)
                        for (int v = 0; v < 3; ++v)
                            if (m.indices[ta * 3 + u] == m.indices[tb * 3 + v]) {
                                adjacent = true;
                                break;
                            }
                    if (adjacent) continue;
                    ++tested;
                    cfloat3 A[3] = {m.positions[m.indices[ta * 3]],
                                    m.positions[m.indices[ta * 3 + 1]],
                                    m.positions[m.indices[ta * 3 + 2]]};
                    cfloat3 B[3] = {m.positions[m.indices[tb * 3]],
                                    m.positions[m.indices[tb * 3 + 1]],
                                    m.positions[m.indices[tb * 3 + 2]]};
                    // slivers have no meaningful interior to penetrate and
                    // their SAT axes degenerate — skip them
                    float area_a = kernel::clength(kernel::ccross(A[1] - A[0], A[2] - A[0]));
                    float area_b = kernel::clength(kernel::ccross(B[1] - B[0], B[2] - B[0]));
                    float min_area = 1e-6f * cell * cell;
                    if (area_a < min_area || area_b < min_area) continue;
                    if (tri_tri_overlap(A, B, cell * 1e-3f)) ++r.intersecting_pairs;
                }
        }
    }
    return r;
}

double signed_volume(const Mesh& m) {
    double v = 0;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        cfloat3 p0 = m.positions[m.indices[t * 3]];
        cfloat3 p1 = m.positions[m.indices[t * 3 + 1]];
        cfloat3 p2 = m.positions[m.indices[t * 3 + 2]];
        v += static_cast<double>(kernel::cdot(p0, kernel::ccross(p1, p2))) / 6.0;
    }
    return v;
}

double surface_area(const Mesh& m) {
    double a = 0;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        cfloat3 p0 = m.positions[m.indices[t * 3]];
        cfloat3 p1 = m.positions[m.indices[t * 3 + 1]];
        cfloat3 p2 = m.positions[m.indices[t * 3 + 2]];
        a += 0.5 * static_cast<double>(kernel::clength(kernel::ccross(p1 - p0, p2 - p0)));
    }
    return a;
}

}  // namespace mesh
}  // namespace clay
