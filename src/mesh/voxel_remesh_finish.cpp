// What a voxel remesh does to the mesh AFTER it comes off the lattice
// (add-voxel-remesher): components, projection, volume, and the cheap source
// measurements the estimate needs.

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "clay/parallel/thread_pool.h"
#include "voxel_remesh_internal.h"

namespace clay {
namespace mesh {
namespace remesh_detail {

using kernel::cf3;

namespace {

// Union-find over vertex indices. Path-halving and union by size, which is the
// usual pair; the only property this needs beyond correctness is that it does
// not depend on iteration order, since components feed a report a test compares
// across runs.
struct DisjointSet {
    std::vector<std::uint32_t> parent;
    std::vector<std::uint32_t> size;

    explicit DisjointSet(std::size_t n) : parent(n), size(n, 1) {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    std::uint32_t find(std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(std::uint32_t a, std::uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (size[a] < size[b]) std::swap(a, b);
        parent[b] = a;
        size[a] += size[b];
    }
};

// Six times the signed volume of the tetrahedron on the origin and one
// triangle. Summed over a closed outward-oriented mesh this is six times its
// volume — the same divergence-theorem identity `mesh::signed_volume` uses,
// kept local because this needs it PER COMPONENT and that function does not
// take a subset.
double tetra_six_volume(kernel::cfloat3 a, kernel::cfloat3 b, kernel::cfloat3 c) {
    return static_cast<double>(a.x) * (static_cast<double>(b.y) * c.z - static_cast<double>(b.z) * c.y) -
           static_cast<double>(a.y) * (static_cast<double>(b.x) * c.z - static_cast<double>(b.z) * c.x) +
           static_cast<double>(a.z) * (static_cast<double>(b.x) * c.y - static_cast<double>(b.y) * c.x);
}

kernel::cfloat3 triangle_normal(const Mesh& m, std::uint32_t tri) {
    const std::size_t base = static_cast<std::size_t>(tri) * 3;
    if (base + 2 >= m.indices.size()) return cf3(0, 0, 0);
    const std::uint32_t ia = m.indices[base], ib = m.indices[base + 1], ic = m.indices[base + 2];
    const std::size_t n = m.positions.size();
    if (ia >= n || ib >= n || ic >= n) return cf3(0, 0, 0);
    const kernel::cfloat3 a = m.positions[ia];
    return kernel::cnormalize(kernel::ccross(m.positions[ib] - a, m.positions[ic] - a));
}

}  // namespace

std::uint32_t label_components(const Mesh& m, std::vector<std::uint32_t>* out_label) {
    const std::size_t vertices = m.positions.size();
    out_label->assign(vertices, 0u);
    if (vertices == 0 || m.indices.empty()) return 0;

    DisjointSet ds(vertices);
    std::vector<std::uint8_t> used(vertices, 0);
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const std::uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        if (a >= vertices || b >= vertices || c >= vertices) continue;
        used[a] = used[b] = used[c] = 1;
        ds.unite(a, b);
        ds.unite(a, c);
    }

    // Numbered in first-appearance order over the vertex array, so the labels
    // are a function of the mesh rather than of a hash map's layout.
    std::unordered_map<std::uint32_t, std::uint32_t> id;
    std::uint32_t next = 0;
    for (std::size_t v = 0; v < vertices; ++v) {
        if (!used[v]) continue;
        const std::uint32_t root = ds.find(static_cast<std::uint32_t>(v));
        auto it = id.find(root);
        if (it == id.end()) it = id.emplace(root, next++).first;
        (*out_label)[v] = it->second;
    }
    return next;
}

std::uint32_t remove_small_components(Mesh* m, double minimum_volume) {
    if (!m || m->indices.empty() || !(minimum_volume > 0.0)) return 0;
    std::vector<std::uint32_t> label;
    const std::uint32_t components = label_components(*m, &label);
    if (components <= 1) return 0;

    std::vector<double> six_volume(components, 0.0);
    const std::size_t vertices = m->positions.size();
    for (std::size_t t = 0; t + 2 < m->indices.size(); t += 3) {
        const std::uint32_t a = m->indices[t], b = m->indices[t + 1], c = m->indices[t + 2];
        // All three, not just the one the label is read through: a marcher
        // never emits an out-of-range index, but this is also reachable from a
        // caller's mesh through the C ABI, and two of the three reads were
        // unguarded.
        if (a >= vertices || b >= vertices || c >= vertices) continue;
        six_volume[label[a]] +=
            tetra_six_volume(m->positions[a], m->positions[b], m->positions[c]);
    }

    std::vector<std::uint8_t> keep(components, 1);
    std::uint32_t removed = 0;
    for (std::uint32_t c = 0; c < components; ++c) {
        if (std::abs(six_volume[c]) / 6.0 >= minimum_volume) continue;
        keep[c] = 0;
        ++removed;
    }
    // NEVER DELETE EVERYTHING. A threshold above the whole model's volume is a
    // caller misjudging the scale, and the useful answer to that is the model
    // back — not an empty mesh that then fails the watertight contract and
    // reports a validation failure for what was really a bad threshold.
    if (removed == 0 || removed == components) return 0;

    // Compact: vertices in their original order, so the surviving geometry is
    // bit-identical to what it was, merely renumbered.
    constexpr std::uint32_t kDropped = 0xFFFFFFFFu;
    std::vector<std::uint32_t> remap(m->positions.size(), kDropped);
    Mesh out;
    const bool has_normals = m->normals.size() == m->positions.size();
    const bool has_colors = m->colors.size() == m->positions.size();
    const bool has_uvs = m->uvs.size() == m->positions.size();
    for (std::size_t t = 0; t + 2 < m->indices.size(); t += 3) {
        const std::uint32_t tri[3] = {m->indices[t], m->indices[t + 1], m->indices[t + 2]};
        if (tri[0] >= m->positions.size() || !keep[label[tri[0]]]) continue;
        for (const std::uint32_t v : tri) {
            if (remap[v] == kDropped) {
                remap[v] = static_cast<std::uint32_t>(out.positions.size());
                out.positions.push_back(m->positions[v]);
                if (has_normals) out.normals.push_back(m->normals[v]);
                if (has_colors) out.colors.push_back(m->colors[v]);
                if (has_uvs) out.uvs.push_back(m->uvs[v]);
            }
            out.indices.push_back(remap[v]);
        }
    }
    // `quads` is deliberately not carried: this rewrites `indices`, and
    // mesh_data.h's rule is that a rewrite must clear them rather than leave a
    // quad list describing triangles that no longer exist. A remesh emits none
    // anyway.
    *m = std::move(out);
    return removed;
}

std::uint64_t project_to_source(Mesh* m, const Mesh& source, const Bvh& source_bvh,
                                float voxel_size, float strength, float max_distance_voxels,
                                parallel::CancelToken* token) {
    if (!m || m->positions.empty() || source_bvh.empty()) return 0;
    if (!(strength > 0.0f) || !(max_distance_voxels > 0.0f)) return 0;

    const float limit = voxel_size * max_distance_voxels;
    const std::vector<kernel::cfloat3> normals = vertex_normals(*m);
    const std::size_t n = m->positions.size();
    std::vector<kernel::cfloat3> moved(n);
    std::vector<std::uint8_t> did(n, 0);

    // Per vertex, disjoint outputs, inputs that are positions only — so the
    // split cannot change a value and the result is bit-identical whatever the
    // pool does with it.
    parallel::for_range(n, 256, [&](std::size_t first, std::size_t last) {
        for (std::size_t v = first; v < last; ++v) {
            const kernel::cfloat3 p = m->positions[v];
            moved[v] = p;
            if (parallel::cancelled(token)) continue;
            const Bvh::ClosestPoint hit = source_bvh.closest(p);
            if (!hit.found || hit.distance > limit) continue;
            // THE SHEET WEIGHT: how much this candidate is trusted, from how
            // well the source faces the way the reconstructed vertex does. Zero
            // where the source faces away, which is the case the contract names
            // — a candidate on a sheet pointing the other way is not this
            // vertex's surface.
            //
            // A WEIGHT AND NOT A REJECTION, and the difference is measured
            // rather than assumed. A hard reject moves a vertex fully and its
            // neighbour not at all, and that discontinuity TEARS: on a sheet
            // folded back through itself at longest-axis 96, projecting with a
            // hard reject left 17 self-intersecting triangle pairs where the
            // unprojected surface had none, and projecting with no test at all
            // also had none. The weight removes the tear (back to zero pairs)
            // while still refusing the back-facing sheet, because it goes to
            // zero continuously instead of falling off a cliff.
            float sheet = 1.0f;
            if (v < normals.size())
                sheet = std::clamp(kernel::cdot(triangle_normal(source, hit.triangle),
                                                normals[v]),
                                   0.0f, 1.0f);
            if (sheet <= 0.0f) continue;
            const kernel::cfloat3 delta = hit.point - p;
            moved[v] = p + delta * (strength * sheet);
            did[v] = 1;
        }
    });
    if (parallel::cancelled(token)) return 0;

    m->positions = std::move(moved);
    std::uint64_t count = 0;
    for (std::uint8_t d : did) count += d;
    return count;
}

SurfaceError measure_surface_error(const Mesh& result, const Bvh& source_bvh) {
    SurfaceError out;
    const std::size_t n = result.positions.size();
    if (n == 0 || source_bvh.empty()) return out;

    std::vector<float> d(n, 0.0f);
    // Per vertex, disjoint outputs, position-only inputs — the same shape the
    // projection uses, so the numbers do not depend on the pool's scheduling.
    parallel::for_range(n, 256, [&](std::size_t first, std::size_t last) {
        for (std::size_t v = first; v < last; ++v)
            d[v] = source_bvh.unsigned_distance(result.positions[v]);
    });

    double sum = 0.0;
    for (float v : d) sum += static_cast<double>(v) * static_cast<double>(v);
    out.rms = std::sqrt(sum / static_cast<double>(n));

    // Sorted rather than a histogram: the array is one float per vertex and a
    // remesh has just built a mesh many times its size, so the sort is noise
    // against what it is measuring — and an exact percentile beats a bucketed
    // one for a number a host will quote.
    std::vector<float> sorted = d;
    std::sort(sorted.begin(), sorted.end());
    out.max = static_cast<double>(sorted.back());
    const std::size_t at = static_cast<std::size_t>(0.95 * static_cast<double>(n - 1));
    out.p95 = static_cast<double>(sorted[at]);
    return out;
}

bool correct_volume(Mesh* m, double target_volume, double current_volume) {
    if (!m || m->positions.empty()) return false;
    if (!(target_volume > 0.0) || !(current_volume > 0.0)) return false;
    const double ratio = std::cbrt(target_volume / current_volume);
    if (!std::isfinite(ratio)) return false;
    const double clamped =
        std::clamp(ratio, 1.0 - static_cast<double>(kVoxelRemeshMaxVolumeCorrection),
                   1.0 + static_cast<double>(kVoxelRemeshMaxVolumeCorrection));
    // Under a thousandth of a per cent is not a correction, it is float noise
    // moving every vertex for nothing.
    if (std::abs(clamped - 1.0) < 1e-5) return false;

    kernel::cfloat3 sum = cf3(0, 0, 0);
    for (const kernel::cfloat3& p : m->positions) sum = sum + p;
    const kernel::cfloat3 centre = sum * (1.0f / static_cast<float>(m->positions.size()));
    const float s = static_cast<float>(clamped);
    for (kernel::cfloat3& p : m->positions) p = centre + (p - centre) * s;
    return true;
}

bool has_thin_features(const Mesh& source, const Bvh& bvh, float voxel_size, float voxels) {
    const std::size_t tris = source.triangle_count();
    if (tris == 0 || bvh.empty()) return false;
    // A bounded, strided walk: the warning must not make the estimate cost what
    // the model costs, and a thin feature that shows on none of 512 spread
    // samples is not one an artist would notice losing.
    constexpr std::size_t kProbes = 512;
    const std::size_t stride = std::max<std::size_t>(1, tris / kProbes);
    const float step = voxel_size * voxels;
    std::size_t probed = 0, thin = 0;
    for (std::size_t t = 0; t < tris; t += stride) {
        const std::size_t base = t * 3;
        const std::uint32_t ia = source.indices[base], ib = source.indices[base + 1],
                            ic = source.indices[base + 2];
        const std::size_t n = source.positions.size();
        if (ia >= n || ib >= n || ic >= n) continue;
        const kernel::cfloat3 a = source.positions[ia];
        const kernel::cfloat3 centroid = (a + source.positions[ib] + source.positions[ic]) *
                                         (1.0f / 3.0f);
        const kernel::cfloat3 nrm = triangle_normal(source, static_cast<std::uint32_t>(t));
        if (kernel::clength(nrm) < 0.5f) continue;  // degenerate: no direction to step
        ++probed;
        // Inward along the face normal. Still inside means there is at least
        // `step` of material behind this face; out means the wall is thinner
        // than the resolution is about to be.
        if (!bvh.is_inside(centroid - nrm * step)) ++thin;
    }
    if (probed == 0) return false;
    // One probe in twenty. Below that it is a chamfer or a sampling artefact
    // rather than a property of the model, and a warning that fires on every
    // model is a warning nobody reads.
    return thin * 20 >= probed;
}

void count_boundaries_and_components(const Mesh& m, std::uint32_t* out_boundary_edges,
                                     std::uint32_t* out_components) {
    if (out_boundary_edges) *out_boundary_edges = 0;
    if (out_components) *out_components = 0;
    if (m.indices.empty()) return;

    if (out_components) {
        std::vector<std::uint32_t> label;
        *out_components = label_components(m, &label);
    }
    if (!out_boundary_edges) return;

    // Undirected edge use counts. An edge used once is a boundary; this
    // deliberately does not compute what `validate` computes, because the
    // estimate has to stay cheap enough for a resolution slider.
    std::unordered_map<std::uint64_t, std::uint32_t> uses;
    uses.reserve(m.indices.size());
    const std::size_t vertices = m.positions.size();
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const std::uint32_t tri[3] = {m.indices[t], m.indices[t + 1], m.indices[t + 2]};
        if (tri[0] >= vertices || tri[1] >= vertices || tri[2] >= vertices) continue;
        for (int e = 0; e < 3; ++e) {
            std::uint32_t a = tri[e], b = tri[(e + 1) % 3];
            if (a > b) std::swap(a, b);
            ++uses[(static_cast<std::uint64_t>(a) << 32) | b];
        }
    }
    std::uint32_t boundary = 0;
    for (const auto& kv : uses)
        if (kv.second == 1) ++boundary;
    *out_boundary_edges = boundary;
}

}  // namespace remesh_detail
}  // namespace mesh
}  // namespace clay
