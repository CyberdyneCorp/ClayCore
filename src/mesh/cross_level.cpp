#include "clay/mesh/cross_level.h"

#include <algorithm>
#include <cmath>

#include "clay/mesh/sculpt_kernels.h"  // safe_normalize
#include "clay/mesh/subdivide.h"

namespace clay {
namespace mesh {
namespace {

using kernel::cfloat3;

// The two corner walks `sculpt.cpp` does over a level's own triangles, over a
// bare triple instead of over a `Mesh`.
//
// COPIED RATHER THAN SHARED, on purpose. The originals take the triangle in the
// index buffer's own corner order and compute `cross(b - a, c - a)`; rotating a
// triangle so the corner of interest comes first is the same value in exact
// arithmetic and NOT the same value in floats. A shared helper would have had
// to rotate, and would have moved the last bits of every existing normal on
// every mesh in the library to fix a boundary nobody outside a regional
// hierarchy has. So the derived faces are summed with the same expressions in
// the same order, and a dense hierarchy's numbers are unchanged.
cfloat3 face_normal(const cfloat3& a, const cfloat3& b, const cfloat3& c) {
    return kernel::ccross(b - a, c - a);
}

float corner_angle(const cfloat3& p, const cfloat3& a, const cfloat3& b) {
    const cfloat3 u = a - p, v = b - p;
    const float lu = kernel::clength(u), lv = kernel::clength(v);
    if (lu < 1e-20f || lv < 1e-20f) return 0.0f;
    return std::acos(std::clamp(kernel::cdot(u, v) / (lu * lv), -1.0f, 1.0f));
}

// A derived quad's two triangles, in the order `level_faces_into` writes them:
// quad (a, b, c, d) is (a, b, c) then (a, c, d). The triangulation has to match
// or a boundary vertex's angle weights are not the dense hierarchy's.
constexpr int kQuadTris[2][3] = {{0, 1, 2}, {0, 2, 3}};

void csr_from_pairs(std::uint32_t rows, std::vector<std::uint32_t>* keys,
                    std::vector<std::uint32_t>* values, std::vector<std::uint32_t>* offsets,
                    std::vector<std::uint32_t>* out) {
    offsets->assign(static_cast<std::size_t>(rows) + 1u, 0u);
    for (std::uint32_t k : *keys) (*offsets)[k + 1]++;
    for (std::size_t i = 1; i < offsets->size(); ++i) (*offsets)[i] += (*offsets)[i - 1];
    out->assign(values->size(), 0u);
    std::vector<std::uint32_t> cursor(*offsets);
    for (std::size_t i = 0; i < keys->size(); ++i) (*out)[cursor[(*keys)[i]]++] = (*values)[i];
}

// The dense child faces of the coarse faces this level does not refine, kept
// when they touch a vertex it does store. In the parent's face order and then
// corner order, which IS the order the dense level numbers them in.
//
// Corners go down in the PARENT'S LAYOUT here and are renumbered once the
// outside set is known, exactly as `subdivide_topology_for_patches` does, because
// the compaction is not known until every face is seen.
void collect_derived_faces(const LevelTopology& parent, const LevelConnectivity& conn,
                           const ChildLayout& layout, const ChildIndex& stored,
                           const std::vector<char>& keep, CrossLevelNeighborhood* out,
                           std::vector<std::uint32_t>* layout_corners) {
    for (std::uint32_t f = 0; f < parent.face_count; ++f) {
        const std::uint32_t patch = parent.patch_of(f);
        if (patch < keep.size() && keep[patch] != 0) continue;
        std::uint32_t arity = 0;
        const std::uint32_t begin = parent.face_begin(f);
        const std::uint32_t* corners = parent.face(f, &arity);
        for (std::uint32_t i = 0; i < arity; ++i) {
            const std::uint32_t e_next = conn.corner_edge[begin + i];
            const std::uint32_t e_prev = conn.corner_edge[begin + (i + arity - 1) % arity];
            const std::uint32_t q[4] = {corners[i], layout.edge_base + e_next,
                                        layout.face_base + f, layout.edge_base + e_prev};
            bool touches = false;
            for (std::uint32_t c : q) touches = touches || stored.stored(c) != kNoVertex;
            if (!touches) continue;
            out->dense_face.push_back(begin + i);
            out->face_patch.push_back(patch);
            for (std::uint32_t c : q) layout_corners->push_back(c);
        }
    }
}

// The two incidences, both over the joined numbering: a face for every corner,
// and the neighbours a derived face gives an id -- its four edges plus the
// diagonal the level mesh's own triangulation adds. Ascending and duplicate
// free per id, the same shape `Adjacency::ring` promises and for the same
// reason: a walk over it has to be deterministic.
void build_incidences(CrossLevelNeighborhood* out) {
    static constexpr int kEdges[5][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 2}};
    const std::uint32_t joined = out->joined_count();
    std::vector<std::uint32_t> keys, values;
    for (std::uint32_t f = 0; f < out->face_count(); ++f)
        for (std::uint32_t k = 0; k < 4; ++k) {
            keys.push_back(out->corners[static_cast<std::size_t>(f) * 4u + k]);
            values.push_back(f);
        }
    csr_from_pairs(joined, &keys, &values, &out->face_offsets, &out->faces);

    keys.clear();
    values.clear();
    for (std::uint32_t f = 0; f < out->face_count(); ++f) {
        const std::uint32_t* q = out->face_corners(f);
        for (const auto& e : kEdges) {
            if (q[e[0]] == q[e[1]]) continue;
            keys.push_back(q[e[0]]);
            values.push_back(q[e[1]]);
            keys.push_back(q[e[1]]);
            values.push_back(q[e[0]]);
        }
    }
    csr_from_pairs(joined, &keys, &values, &out->ring_offsets, &out->ring);

    std::vector<std::uint32_t> offsets(out->ring_offsets.size(), 0u), packed;
    packed.reserve(out->ring.size());
    for (std::uint32_t v = 0; v < joined; ++v) {
        const std::uint32_t begin = out->ring_offsets[v], end = out->ring_offsets[v + 1];
        std::sort(out->ring.begin() + begin, out->ring.begin() + end);
        offsets[v] = static_cast<std::uint32_t>(packed.size());
        for (std::uint32_t i = begin; i < end; ++i)
            if (i == begin || out->ring[i] != out->ring[i - 1]) packed.push_back(out->ring[i]);
    }
    offsets[joined] = static_cast<std::uint32_t>(packed.size());
    out->ring_offsets = std::move(offsets);
    out->ring = std::move(packed);
}

}  // namespace

cfloat3 CrossLevelNeighborhood::normal_contribution(const std::vector<cfloat3>& own,
                                                    std::uint32_t joined) const {
    std::size_t count = 0;
    const std::uint32_t* incident = faces_of(joined, &count);
    cfloat3 sum = kernel::cf3(0, 0, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t* q = face_corners(incident[i]);
        for (const auto& tri : kQuadTris) {
            const cfloat3 p0 = position(own, q[tri[0]]);
            const cfloat3 p1 = position(own, q[tri[1]]);
            const cfloat3 p2 = position(own, q[tri[2]]);
            const cfloat3 face = safe_normalize(face_normal(p0, p1, p2), kernel::cf3(0, 0, 0));
            const cfloat3 p[3] = {p0, p1, p2};
            for (int corner = 0; corner < 3; ++corner)
                if (q[tri[corner]] == joined)
                    sum = sum + face * corner_angle(p[corner], p[(corner + 1) % 3],
                                                    p[(corner + 2) % 3]);
        }
    }
    return sum;
}

int CrossLevelNeighborhood::shared_triangles(std::uint32_t a, std::uint32_t b) const {
    std::size_t count = 0;
    const std::uint32_t* incident = faces_of(a, &count);
    int shared = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t* q = face_corners(incident[i]);
        for (const auto& tri : kQuadTris) {
            bool has_a = false, has_b = false;
            for (int corner = 0; corner < 3; ++corner) {
                has_a = has_a || q[tri[corner]] == a;
                has_b = has_b || q[tri[corner]] == b;
            }
            if (has_a && has_b) ++shared;
        }
    }
    return shared;
}

std::size_t CrossLevelNeighborhood::bytes() const {
    const auto v32 = [](const std::vector<std::uint32_t>& x) {
        return x.capacity() * sizeof(std::uint32_t);
    };
    const auto v3 = [](const std::vector<cfloat3>& x) { return x.capacity() * sizeof(cfloat3); };
    return v32(outside_layout) + v3(outside_positions) + v32(corners) + v32(dense_face) +
           v32(face_patch) + v32(face_offsets) + v32(faces) + v32(ring_offsets) + v32(ring);
}

CrossLevelNeighborhood build_cross_level(const LevelTopology& parent,
                                         const LevelConnectivity& parent_conn,
                                         const std::vector<cfloat3>& parent_positions,
                                         const LevelTopology& child,
                                         const std::vector<char>& keep) {
    CrossLevelNeighborhood out;
    out.vertex_count = child.vertex_count;
    // A level that stores every patch has a complete connectivity of its own,
    // so there is nothing outside it and no indirection to pay for saying so.
    if (keep.empty() || child.dense()) return out;

    const ChildLayout layout = ChildLayout::of(parent, parent_conn);
    const ChildIndex stored = ChildIndex::of(child);
    std::vector<std::uint32_t> layout_corners;  // 4 per derived face, in the parent's layout
    collect_derived_faces(parent, parent_conn, layout, stored, keep, &out, &layout_corners);
    if (out.dense_face.empty()) return out;

    // The corners this level does not store, ascending, and the pure
    // subdivision's value at each. Through `subdivide_positions` itself rather
    // than a second copy of the four rules — the same call the level's own
    // vertices came out of, so a boundary value is the dense hierarchy's bits
    // and not a reimplementation of them.
    out.outside_layout.clear();
    for (std::uint32_t c : layout_corners)
        if (stored.stored(c) == kNoVertex) out.outside_layout.push_back(c);
    std::sort(out.outside_layout.begin(), out.outside_layout.end());
    out.outside_layout.erase(std::unique(out.outside_layout.begin(), out.outside_layout.end()),
                             out.outside_layout.end());
    refresh_cross_level(parent, parent_conn, parent_positions, &out);

    // The corners in the joined numbering, which is what every reader sees.
    ChildIndex outside;
    outside.full_of = out.outside_layout.data();
    outside.count = static_cast<std::uint32_t>(out.outside_layout.size());
    out.corners.resize(layout_corners.size());
    for (std::size_t i = 0; i < layout_corners.size(); ++i) {
        const std::uint32_t s = stored.stored(layout_corners[i]);
        out.corners[i] = s != kNoVertex ? s : out.vertex_count + outside.stored(layout_corners[i]);
    }

    build_incidences(&out);
    return out;
}

void refresh_cross_level(const LevelTopology& parent, const LevelConnectivity& parent_conn,
                         const std::vector<cfloat3>& parent_positions,
                         CrossLevelNeighborhood* inout) {
    if (!inout || inout->outside_layout.empty()) return;
    // THE STENCILS THEMSELVES, over a `ChildIndex` that stores exactly the
    // outside vertices. `subdivide_positions` writes `out[i]` for the layout
    // vertex `full(i)` with the same expression it uses for a level's own
    // vertices, so an outside position is the dense hierarchy's bits rather
    // than a second implementation of the four rules.
    ChildIndex outside;
    outside.full_of = inout->outside_layout.data();
    outside.count = static_cast<std::uint32_t>(inout->outside_layout.size());
    subdivide_positions(parent, parent_conn, parent_positions, outside, &inout->outside_positions);
}

}  // namespace mesh
}  // namespace clay
