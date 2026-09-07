#include "clay/mesh/subdivide.h"

#include <array>

#include <algorithm>
#include <cstring>

#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {
namespace {

// A corner's edge, before the edges have identities. Sorted by (a, b) and then
// by (face, corner) so that an edge's two faces are found in ascending face
// order — which is what makes `f0` and `f1` the same on every run rather than
// whichever the container happened to visit first.
struct CornerEdge {
    std::uint32_t a = 0, b = 0;
    std::uint32_t face = 0;
    std::uint32_t corner = 0;

    bool same_edge(const CornerEdge& o) const { return a == o.a && b == o.b; }
    bool operator<(const CornerEdge& o) const {
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        if (face != o.face) return face < o.face;
        return corner < o.corner;
    }
};

// Counting sort into CSR: count, prefix-sum, fill. Written once because the
// two incidence tables below both want it and a second copy is a second chance
// to get the prefix sum off by one.
void csr_from_pairs(std::uint32_t key_count, const std::vector<std::uint32_t>& keys,
                    const std::vector<std::uint32_t>& values,
                    std::vector<std::uint32_t>* offsets, std::vector<std::uint32_t>* out) {
    offsets->assign(static_cast<std::size_t>(key_count) + 1, 0);
    for (std::uint32_t k : keys) (*offsets)[k + 1]++;
    for (std::size_t i = 1; i < offsets->size(); ++i) (*offsets)[i] += (*offsets)[i - 1];
    out->assign(values.size(), 0);
    std::vector<std::uint32_t> cursor(*offsets);
    for (std::size_t i = 0; i < keys.size(); ++i) (*out)[cursor[keys[i]]++] = values[i];
}

bool face_has_duplicate_corner(const std::uint32_t* corners, std::uint32_t arity) {
    for (std::uint32_t i = 0; i < arity; ++i)
        for (std::uint32_t j = i + 1; j < arity; ++j)
            if (corners[i] == corners[j]) return true;
    return false;
}

}  // namespace

std::size_t LevelTopology::bytes() const {
    return corners.capacity() * sizeof(std::uint32_t) +
           face_offsets.capacity() * sizeof(std::uint32_t) +
           face_patch.capacity() * sizeof(std::uint32_t) +
           full_of.capacity() * sizeof(std::uint32_t);
}

std::size_t LevelConnectivity::bytes() const {
    return edges.capacity() * sizeof(LevelEdge) + corner_edge.capacity() * sizeof(std::uint32_t) +
           vertex_face_offsets.capacity() * sizeof(std::uint32_t) +
           vertex_faces.capacity() * sizeof(std::uint32_t) +
           vertex_edge_offsets.capacity() * sizeof(std::uint32_t) +
           vertex_edges.capacity() * sizeof(std::uint32_t);
}

LevelConnectivity LevelConnectivity::build(const LevelTopology& topology) {
    LevelConnectivity c;

    std::vector<CornerEdge> pairs;
    pairs.reserve(topology.corners.size());
    for (std::uint32_t f = 0; f < topology.face_count; ++f) {
        std::uint32_t arity = 0;
        const std::uint32_t begin = topology.face_begin(f);
        const std::uint32_t* corners = topology.face(f, &arity);
        for (std::uint32_t i = 0; i < arity; ++i) {
            const std::uint32_t v0 = corners[i];
            const std::uint32_t v1 = corners[(i + 1) % arity];
            CornerEdge ce;
            ce.a = std::min(v0, v1);
            ce.b = std::max(v0, v1);
            ce.face = f;
            ce.corner = begin + i;
            pairs.push_back(ce);
        }
    }
    std::sort(pairs.begin(), pairs.end());

    c.corner_edge.assign(topology.corners.size(), kNoEdge);
    c.edges.reserve(pairs.size() / 2 + 1);
    std::size_t i = 0;
    while (i < pairs.size()) {
        std::size_t j = i + 1;
        while (j < pairs.size() && pairs[j].same_edge(pairs[i])) ++j;
        LevelEdge e;
        e.a = pairs[i].a;
        e.b = pairs[i].b;
        e.f0 = pairs[i].face;
        if (j - i >= 2) e.f1 = pairs[i + 1].face;
        if (j - i > 2) c.non_manifold = true;
        const std::uint32_t id = static_cast<std::uint32_t>(c.edges.size());
        for (std::size_t k = i; k < j; ++k) c.corner_edge[pairs[k].corner] = id;
        c.edges.push_back(e);
        i = j;
    }

    // vertex -> faces, in ascending face order.
    {
        std::vector<std::uint32_t> keys, values;
        keys.reserve(topology.corners.size());
        values.reserve(topology.corners.size());
        for (std::uint32_t f = 0; f < topology.face_count; ++f) {
            std::uint32_t arity = 0;
            const std::uint32_t* corners = topology.face(f, &arity);
            for (std::uint32_t k = 0; k < arity; ++k) {
                keys.push_back(corners[k]);
                values.push_back(f);
            }
        }
        csr_from_pairs(topology.vertex_count, keys, values, &c.vertex_face_offsets,
                       &c.vertex_faces);
    }

    // vertex -> edges, in ascending edge order.
    {
        std::vector<std::uint32_t> keys, values;
        keys.reserve(c.edges.size() * 2);
        values.reserve(c.edges.size() * 2);
        for (std::uint32_t e = 0; e < c.edges.size(); ++e) {
            keys.push_back(c.edges[e].a);
            values.push_back(e);
            keys.push_back(c.edges[e].b);
            values.push_back(e);
        }
        csr_from_pairs(topology.vertex_count, keys, values, &c.vertex_edge_offsets,
                       &c.vertex_edges);
    }

    return c;
}

std::uint32_t ChildIndex::stored(std::uint32_t full) const {
    if (dense()) return full < count ? full : kNoVertex;
    const std::uint32_t* end = full_of + count;
    const std::uint32_t* at = std::lower_bound(full_of, end, full);
    if (at == end || *at != full) return kNoVertex;
    return static_cast<std::uint32_t>(at - full_of);
}

LevelTopology subdivide_topology(const LevelTopology& parent, const LevelConnectivity& conn) {
    const ChildLayout layout = ChildLayout::of(parent, conn);

    LevelTopology child;
    child.vertex_count = layout.total;
    child.face_count = static_cast<std::uint32_t>(parent.corners.size());
    child.corners.resize(static_cast<std::size_t>(child.face_count) * 4u);
    child.face_patch.resize(child.face_count);
    child.patch_count = parent.face_patch.empty() ? parent.face_count : parent.patch_count;

    std::size_t out = 0;
    for (std::uint32_t f = 0; f < parent.face_count; ++f) {
        std::uint32_t arity = 0;
        const std::uint32_t begin = parent.face_begin(f);
        const std::uint32_t* corners = parent.face(f, &arity);
        const std::uint32_t patch = parent.patch_of(f);
        for (std::uint32_t i = 0; i < arity; ++i) {
            const std::uint32_t e_next = conn.corner_edge[begin + i];
            const std::uint32_t e_prev = conn.corner_edge[begin + (i + arity - 1) % arity];
            child.corners[out * 4 + 0] = corners[i];
            child.corners[out * 4 + 1] = layout.edge_base + e_next;
            child.corners[out * 4 + 2] = layout.face_base + f;
            child.corners[out * 4 + 3] = layout.edge_base + e_prev;
            child.face_patch[out] = patch;
            ++out;
        }
    }
    return child;
}

LevelTopology subdivide_topology_for_patches(const LevelTopology& parent,
                                             const LevelConnectivity& conn,
                                             const std::vector<char>& keep) {
    const ChildLayout layout = ChildLayout::of(parent, conn);

    LevelTopology child;
    child.patch_count = parent.face_patch.empty() ? parent.face_count : parent.patch_count;

    // The refined faces IN THE PARENT'S OWN FACE ORDER, which is what keeps a
    // level patch-major -- and with it the chunk table's identity, which a host
    // has already uploaded by.
    //
    // Corners go down in the parent's layout here and are renumbered once at
    // the end, because the compaction is not known until every face is seen.
    for (std::uint32_t f = 0; f < parent.face_count; ++f) {
        const std::uint32_t patch = parent.patch_of(f);
        if (patch >= keep.size() || !keep[patch]) continue;
        std::uint32_t arity = 0;
        const std::uint32_t begin = parent.face_begin(f);
        const std::uint32_t* corners = parent.face(f, &arity);
        for (std::uint32_t i = 0; i < arity; ++i) {
            const std::uint32_t e_next = conn.corner_edge[begin + i];
            const std::uint32_t e_prev = conn.corner_edge[begin + (i + arity - 1) % arity];
            child.corners.push_back(corners[i]);
            child.corners.push_back(layout.edge_base + e_next);
            child.corners.push_back(layout.face_base + f);
            child.corners.push_back(layout.edge_base + e_prev);
            child.face_patch.push_back(patch);
        }
    }
    child.face_count = static_cast<std::uint32_t>(child.face_patch.size());

    // EVERY face kept is the dense level, and it is emitted as the dense level
    // rather than as a subset that happens to be complete: an identity
    // `full_of` would cost four bytes a vertex and an indirection per lookup to
    // say nothing.
    if (child.corners.size() == parent.corners.size() * 4u) {
        child.vertex_count = layout.total;
        return child;
    }

    // Compact to the vertices those faces reference. ASCENDING, which is what
    // makes `ChildIndex::stored` a binary search and what makes two runs of the
    // same request produce the same numbering rather than a numbering that
    // follows the order faces happened to be visited in.
    child.full_of = child.corners;
    std::sort(child.full_of.begin(), child.full_of.end());
    child.full_of.erase(std::unique(child.full_of.begin(), child.full_of.end()),
                        child.full_of.end());
    child.vertex_count = static_cast<std::uint32_t>(child.full_of.size());
    const ChildIndex index = ChildIndex::of(child);
    for (std::uint32_t& c : child.corners) c = index.stored(c);
    return child;
}

std::size_t LevelHalo::bytes() const {
    return faces.bytes() + full_of.capacity() * sizeof(std::uint32_t) +
           positions.capacity() * sizeof(kernel::cfloat3) +
           vertex_face_offsets.capacity() * sizeof(std::uint32_t) +
           vertex_faces.capacity() * sizeof(std::uint32_t);
}

namespace {

// The layout id of every vertex a level stores, reversed into a direct lookup.
//
// A DIRECT MAP rather than `ChildIndex::stored`'s binary search, and that is a
// measurement rather than a preference: the search is consulted sixteen times
// per parent face and costs a dozen steps each, and it was most of what this
// whole neighbourhood took to build -- +71% on a full level evaluation before
// this replaced it, +13% after.
//
// Sized by the PARENT's layout, not by the dense surface, so it stays small for
// exactly the reason a regional level exists: the parent of a regional level is
// itself regional. Transient: it is dropped when the halo is built.
std::vector<std::uint32_t> stored_by_layout_id(const ChildLayout& layout,
                                               const LevelTopology& child) {
    std::vector<std::uint32_t> out(layout.total, kNoVertex);
    for (std::uint32_t i = 0; i < child.vertex_count; ++i) {
        const std::uint32_t full = child.dense() ? i : child.full_of[i];
        if (full < out.size()) out[full] = i;
    }
    return out;
}

// The child quads of the parent faces `keep` did NOT refine, corners still in
// the layout's numbering, and only where a quad reaches a vertex the level
// stores.
//
// Emitted by the same expression `subdivide_topology_for_patches` uses, in the
// same corner order, so a halo face IS the dense level's face rather than an
// approximation of one. A quad reaching no stored vertex contributes to no
// stored vertex's normal, and keeping it would make this cost the surface
// outside the region rather than the region's edge.
// The child quad at corner `i` of parent face `f`, in the layout's numbering:
// the corner's own vertex point, the edge point ahead of it, the face point,
// and the edge point behind it. THE EXPRESSION `subdivide_topology_for_patches`
// USES, and it is here rather than duplicated so the two cannot drift.
std::array<std::uint32_t, 4> child_quad_of(const LevelTopology& parent,
                                           const LevelConnectivity& conn,
                                           const ChildLayout& layout, std::uint32_t f,
                                           std::uint32_t i, std::uint32_t arity) {
    const std::uint32_t begin = parent.face_begin(f);
    std::uint32_t out_arity = 0;
    const std::uint32_t* corners = parent.face(f, &out_arity);
    return {corners[i], layout.edge_base + conn.corner_edge[begin + i], layout.face_base + f,
            layout.edge_base + conn.corner_edge[begin + (i + arity - 1) % arity]};
}

bool reaches_level(const std::array<std::uint32_t, 4>& quad,
                   const std::vector<std::uint32_t>& stored_of) {
    for (std::uint32_t c : quad)
        if (stored_of[c] != kNoVertex) return true;
    return false;
}

std::vector<std::uint32_t> halo_layout_corners(const LevelTopology& parent,
                                               const LevelConnectivity& conn,
                                               const std::vector<char>& keep,
                                               const ChildLayout& layout,
                                               const std::vector<std::uint32_t>& stored_of) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t f = 0; f < parent.face_count; ++f) {
        const std::uint32_t patch = parent.patch_of(f);
        if (patch < keep.size() && keep[patch]) continue;  // refined: already stored
        const std::uint32_t arity = parent.face_arity(f);
        for (std::uint32_t i = 0; i < arity; ++i) {
            const std::array<std::uint32_t, 4> quad =
                child_quad_of(parent, conn, layout, f, i, arity);
            if (!reaches_level(quad, stored_of)) continue;
            out.insert(out.end(), quad.begin(), quad.end());
        }
    }
    return out;
}

// The halo faces touching each STORED vertex, as a CSR. Over the stored
// vertices only: that is what a normal sum indexes by, and an entry for a
// halo-only vertex would be storage nothing reads.
void build_halo_incidences(LevelHalo* halo) {
    halo->vertex_face_offsets.assign(halo->stored_count + 1, 0);
    for (std::uint32_t f = 0; f < halo->faces.face_count; ++f)
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::uint32_t v = halo->faces.corners[f * 4 + i];
            if (v < halo->stored_count) halo->vertex_face_offsets[v + 1]++;
        }
    for (std::uint32_t v = 0; v < halo->stored_count; ++v)
        halo->vertex_face_offsets[v + 1] += halo->vertex_face_offsets[v];
    halo->vertex_faces.resize(halo->vertex_face_offsets[halo->stored_count]);
    std::vector<std::uint32_t> cursor(halo->vertex_face_offsets.begin(),
                                      halo->vertex_face_offsets.end() - 1);
    for (std::uint32_t f = 0; f < halo->faces.face_count; ++f)
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::uint32_t v = halo->faces.corners[f * 4 + i];
            if (v < halo->stored_count) halo->vertex_faces[cursor[v]++] = f;
        }
}

}  // namespace

LevelHalo build_level_halo(const LevelTopology& parent, const LevelConnectivity& conn,
                           const std::vector<char>& keep, const LevelTopology& child) {
    LevelHalo halo;
    halo.stored_count = child.vertex_count;
    // A level that stores every vertex its parent's layout defines has no ring
    // outside itself, and neither has one built from an empty `keep` -- that is
    // the dense path, where `subdivide_topology_for_patches` forwards to
    // `subdivide_topology`.
    if (child.dense() || keep.empty()) return halo;

    const ChildLayout layout = ChildLayout::of(parent, conn);
    const std::vector<std::uint32_t> stored_of = stored_by_layout_id(layout, child);
    const std::vector<std::uint32_t> layout_corners =
        halo_layout_corners(parent, conn, keep, layout, stored_of);
    if (layout_corners.empty()) return halo;

    // The halo-only vertices, ascending and unique, so the numbering is a
    // function of the topology rather than of the order faces were visited in.
    for (std::uint32_t c : layout_corners)
        if (stored_of[c] == kNoVertex) halo.full_of.push_back(c);
    std::sort(halo.full_of.begin(), halo.full_of.end());
    halo.full_of.erase(std::unique(halo.full_of.begin(), halo.full_of.end()), halo.full_of.end());
    const ChildIndex halo_index = halo.index();

    halo.faces.corners.resize(layout_corners.size());
    for (std::size_t i = 0; i < layout_corners.size(); ++i) {
        const std::uint32_t stored = stored_of[layout_corners[i]];
        halo.faces.corners[i] = stored != kNoVertex
                                    ? stored
                                    : halo.stored_count + halo_index.stored(layout_corners[i]);
    }
    halo.faces.face_count = static_cast<std::uint32_t>(layout_corners.size() / 4);
    halo.faces.vertex_count = halo.stored_count + halo.halo_vertex_count();
    halo.positions.assign(halo.halo_vertex_count(), kernel::cf3(0, 0, 0));
    build_halo_incidences(&halo);
    return halo;
}

namespace {

// The four Catmull-Clark rules, over any value type that adds and scales. Used
// for positions and, over its own connectivity, for UVs and colours — the same
// stencils, so a subdivided attribute and a subdivided position cannot disagree
// about what "the point on this edge" means.
template <class V>
V face_point_of(const LevelTopology& parent, const std::vector<V>& P, std::uint32_t f) {
    std::uint32_t arity = 0;
    const std::uint32_t* corners = parent.face(f, &arity);
    V sum = P[corners[0]];
    for (std::uint32_t i = 1; i < arity; ++i) sum = sum + P[corners[i]];
    return sum / static_cast<float>(arity);
}

template <class V, class FaceFn>
V edge_point_of(const LevelConnectivity& conn, const std::vector<V>& P, std::uint32_t e,
                FaceFn face_point) {
    const LevelEdge& edge = conn.edges[e];
    const V mid = (P[edge.a] + P[edge.b]) * 0.5f;
    // AN OPEN BORDER IS THE MIDPOINT, and applying the interior average there
    // instead is the classic way to watch a bordered patch shrink away from
    // where the artist put it.
    if (edge.boundary()) return mid;
    return (P[edge.a] + P[edge.b] + face_point(edge.f0) + face_point(edge.f1)) * 0.25f;
}

// The boundary vertex rule: (1, 6, 1) / 8 along the border, and a vertex with
// only one incident face held exactly where it is, because that is a corner of
// the sheet and rounding it off would move the silhouette of every open mesh.
template <class V>
V boundary_vertex_point(const LevelConnectivity& conn, const std::vector<V>& P, std::uint32_t v,
                        const std::uint32_t* edges, std::size_t edge_count, std::size_t face_count) {
    if (face_count <= 1) return P[v];
    std::uint32_t n0 = kNoVertex, n1 = kNoVertex;
    std::size_t found = 0;
    for (std::size_t i = 0; i < edge_count; ++i) {
        const LevelEdge& e = conn.edges[edges[i]];
        if (!e.boundary()) continue;
        const std::uint32_t other = (e.a == v) ? e.b : e.a;
        if (found == 0)
            n0 = other;
        else if (found == 1)
            n1 = other;
        ++found;
    }
    // More than two border edges is a pinch, not a border: there is no "the two
    // neighbours along the boundary" to average, so the vertex stays put rather
    // than being smoothed toward an arbitrary pair.
    if (found != 2) return P[v];
    return (P[n0] + P[v] * 6.0f + P[n1]) * 0.125f;
}

template <class V, class FaceFn>
V interior_vertex_point(const LevelConnectivity& conn, const std::vector<V>& P, std::uint32_t v,
                        const std::uint32_t* edges, std::size_t edge_count,
                        const std::uint32_t* faces, std::size_t face_count, FaceFn face_point) {
    const float n = static_cast<float>(edge_count);
    if (edge_count < 3 || face_count == 0) return P[v];

    V fsum = face_point(faces[0]);
    for (std::size_t i = 1; i < face_count; ++i) fsum = fsum + face_point(faces[i]);
    const V favg = fsum / static_cast<float>(face_count);

    V rsum = (P[conn.edges[edges[0]].a] + P[conn.edges[edges[0]].b]) * 0.5f;
    for (std::size_t i = 1; i < edge_count; ++i) {
        const LevelEdge& e = conn.edges[edges[i]];
        rsum = rsum + (P[e.a] + P[e.b]) * 0.5f;
    }
    const V ravg = rsum / n;

    return (favg + ravg * 2.0f + P[v] * (n - 3.0f)) / n;
}

template <class V, class FaceFn>
V vertex_point_of(const LevelConnectivity& conn, const std::vector<V>& P, std::uint32_t v,
                  FaceFn face_point) {
    std::size_t edge_count = 0, face_count = 0;
    const std::uint32_t* edges = conn.edges_of(v, &edge_count);
    const std::uint32_t* faces = conn.faces_of(v, &face_count);
    if (edge_count == 0) return P[v];

    bool on_boundary = false;
    for (std::size_t i = 0; i < edge_count && !on_boundary; ++i)
        on_boundary = conn.edges[edges[i]].boundary();

    if (on_boundary)
        return boundary_vertex_point(conn, P, v, edges, edge_count, face_count);
    return interior_vertex_point(conn, P, v, edges, edge_count, faces, face_count, face_point);
}

template <class V, class FaceFn>
V child_value_of(std::uint32_t child, const ChildLayout& layout, const LevelConnectivity& conn,
                 const std::vector<V>& P, FaceFn face_point) {
    switch (layout.origin_of(child)) {
        case SubdivisionOrigin::VertexPoint:
            return vertex_point_of(conn, P, child, face_point);
        case SubdivisionOrigin::EdgePoint:
            return edge_point_of(conn, P, child - layout.edge_base, face_point);
        default:
            return face_point(child - layout.face_base);
    }
}

// The whole level. Face points are computed ONCE into a scratch array here,
// because every one of them is read by four edge points and four vertex points
// and recomputing it nine times is nine times the arithmetic.
template <class V>
void subdivide_all(const LevelTopology& parent, const LevelConnectivity& conn,
                   const std::vector<V>& P, ChildIndex child, std::vector<V>* out) {
    const ChildLayout layout = ChildLayout::of(parent, conn);
    std::vector<V> fp(parent.face_count);
    for (std::uint32_t f = 0; f < parent.face_count; ++f) fp[f] = face_point_of(parent, P, f);
    const auto face_point = [&fp](std::uint32_t f) { return fp[f]; };

    out->resize(child.count);
    for (std::uint32_t c = 0; c < child.count; ++c)
        (*out)[c] = child_value_of(child.full(c), layout, conn, P, face_point);
}

// The whole layout, which is what a dense level stores.
ChildIndex whole(const LevelTopology& parent, const LevelConnectivity& conn) {
    ChildIndex idx;
    idx.count = ChildLayout::of(parent, conn).total;
    return idx;
}

}  // namespace

void subdivide_positions(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat3>& parent_positions,
                         std::vector<kernel::cfloat3>* out) {
    subdivide_all(parent, conn, parent_positions, whole(parent, conn), out);
}

void subdivide_positions(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat3>& parent_positions, ChildIndex child,
                         std::vector<kernel::cfloat3>* out) {
    subdivide_all(parent, conn, parent_positions, child, out);
}

void subdivide_attribute(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat2>& in,
                         std::vector<kernel::cfloat2>* out) {
    subdivide_all(parent, conn, in, whole(parent, conn), out);
}

void subdivide_attribute(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat3>& in,
                         std::vector<kernel::cfloat3>* out) {
    subdivide_all(parent, conn, in, whole(parent, conn), out);
}

void subdivide_attribute(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat2>& in, ChildIndex child,
                         std::vector<kernel::cfloat2>* out) {
    subdivide_all(parent, conn, in, child, out);
}

void subdivide_attribute(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat3>& in, ChildIndex child,
                         std::vector<kernel::cfloat3>* out) {
    subdivide_all(parent, conn, in, child, out);
}

void subdivide_positions_partial(const LevelTopology& parent, const LevelConnectivity& conn,
                                 const std::vector<kernel::cfloat3>& parent_positions,
                                 ChildIndex child,
                                 const std::vector<std::uint32_t>& child_vertices,
                                 std::vector<kernel::cfloat3>* inout) {
    const ChildLayout layout = ChildLayout::of(parent, conn);
    if (inout->size() < child.count) inout->resize(child.count);
    // Face points ON DEMAND here, which is the opposite trade from the whole
    // level: a partial pass touches a handful of faces, and a scratch array
    // over every face would make the call proportional to the LEVEL — the one
    // thing this entry point exists to avoid.
    const auto face_point = [&](std::uint32_t f) {
        return face_point_of(parent, parent_positions, f);
    };
    for (std::uint32_t c : child_vertices)
        (*inout)[c] = child_value_of(child.full(c), layout, conn, parent_positions, face_point);
}

void subdivide_positions_partial(const LevelTopology& parent, const LevelConnectivity& conn,
                                 const std::vector<kernel::cfloat3>& parent_positions,
                                 const std::vector<std::uint32_t>& child_vertices,
                                 std::vector<kernel::cfloat3>* inout) {
    subdivide_positions_partial(parent, conn, parent_positions, whole(parent, conn),
                                child_vertices, inout);
}

void dirty_children(const LevelTopology& parent, const LevelConnectivity& conn,
                    const std::vector<std::uint32_t>& dirty_parents,
                    std::vector<std::uint32_t>* out) {
    const ChildLayout layout = ChildLayout::of(parent, conn);
    out->clear();
    for (std::uint32_t p : dirty_parents) {
        if (p >= parent.vertex_count) continue;
        std::size_t face_count = 0;
        const std::uint32_t* faces = conn.faces_of(p, &face_count);
        for (std::size_t i = 0; i < face_count; ++i) {
            const std::uint32_t f = faces[i];
            std::uint32_t arity = 0;
            const std::uint32_t begin = parent.face_begin(f);
            const std::uint32_t* corners = parent.face(f, &arity);
            out->push_back(layout.face_base + f);
            for (std::uint32_t k = 0; k < arity; ++k) {
                out->push_back(corners[k]);
                out->push_back(layout.edge_base + conn.corner_edge[begin + k]);
            }
        }
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
}

void dirty_children(const LevelTopology& parent, const LevelConnectivity& conn,
                    const std::vector<std::uint32_t>& dirty_parents, ChildIndex child,
                    std::vector<std::uint32_t>* out) {
    dirty_children(parent, conn, dirty_parents, out);
    if (child.dense()) return;
    // Translate in place. The list ascends and so does `full_of`, so this is a
    // merge rather than a search per entry.
    std::size_t write = 0, at = 0;
    for (std::uint32_t full : *out) {
        while (at < child.count && child.full_of[at] < full) ++at;
        if (at == child.count) break;
        if (child.full_of[at] == full) (*out)[write++] = static_cast<std::uint32_t>(at);
    }
    out->resize(write);
}

bool base_topology_from_mesh(const Mesh& m, const std::uint32_t* class_of,
                             std::uint32_t class_count, LevelTopology* out) {
    if (!out || !class_of || class_count == 0) return false;
    const std::uint32_t vertex_count = static_cast<std::uint32_t>(m.positions.size());
    if (vertex_count == 0) return false;

    // The cage is the QUADS when the mesh has them — mesh_data.h guarantees the
    // triangles are exactly their triangulation, so reading the triangles
    // instead would subdivide a diagonal the author never drew.
    const bool quads = m.has_quads();
    const std::vector<std::uint32_t>& src = quads ? m.quads : m.indices;
    const std::uint32_t arity = quads ? 4u : 3u;
    if (src.empty() || src.size() % arity != 0) return false;

    LevelTopology t;
    t.vertex_count = class_count;
    t.face_count = static_cast<std::uint32_t>(src.size() / arity);
    t.patch_count = t.face_count;
    t.corners.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        const std::uint32_t raw = src[i];
        if (raw >= vertex_count) return false;
        const std::uint32_t cls = class_of[raw];
        if (cls >= class_count) return false;
        t.corners[i] = cls;
    }
    // A face whose corners weld together is not a face of this arity, and
    // subdividing it would produce a quad with two coincident corners at every
    // level above. Refused rather than repaired: repairing it silently changes
    // the cage the artist retopologised.
    for (std::uint32_t f = 0; f < t.face_count; ++f)
        if (face_has_duplicate_corner(t.corners.data() + f * arity, arity)) return false;
    // `face_offsets` empty MEANS quads (see the header); a triangle cage has to
    // say its arity explicitly.
    if (!quads) {
        t.face_offsets.resize(static_cast<std::size_t>(t.face_count) + 1);
        for (std::uint32_t f = 0; f <= t.face_count; ++f) t.face_offsets[f] = f * 3u;
    }
    *out = std::move(t);
    return true;
}

void level_faces_into(const LevelTopology& topology, Mesh* out) {
    if (!out) return;
    out->indices.clear();
    out->quads.clear();
    if (topology.uniform_quads()) {
        out->quads = topology.corners;
        out->indices.reserve(static_cast<std::size_t>(topology.face_count) * 6u);
        for (std::uint32_t f = 0; f < topology.face_count; ++f) {
            const std::uint32_t* c = topology.corners.data() + f * 4u;
            out->indices.insert(out->indices.end(), {c[0], c[1], c[2], c[0], c[2], c[3]});
        }
        return;
    }
    // A triangle or mixed cage: fan-triangulate and write NO quads, because a
    // quad list that does not describe `indices` is the lie mesh_data.h forbids.
    for (std::uint32_t f = 0; f < topology.face_count; ++f) {
        std::uint32_t arity = 0;
        const std::uint32_t* c = topology.face(f, &arity);
        for (std::uint32_t i = 2; i < arity; ++i)
            out->indices.insert(out->indices.end(), {c[0], c[i - 1], c[i]});
    }
}

void level_to_mesh(const LevelTopology& topology, const std::vector<kernel::cfloat3>& positions,
                   Mesh* out) {
    if (!out) return;
    out->positions = positions;
    out->normals.clear();
    out->colors.clear();
    out->uvs.clear();
    level_faces_into(topology, out);
}

}  // namespace mesh
}  // namespace clay
