#include "clay/mesh/surface_frame.h"

#include <cmath>

namespace clay {
namespace mesh {
namespace {

using kernel::cfloat3;

cfloat3 safe_unit(cfloat3 v, cfloat3 fallback) {
    const float len2 = kernel::cdot(v, v);
    if (!(len2 > 1e-24f)) return fallback;
    return v / std::sqrt(len2);
}

// A direction perpendicular to `n`, chosen without a threshold that could snap.
// The branch is on the SMALLEST component of the normal, which changes only
// where two components are exactly equal — and there both choices give the same
// perpendicular family.
cfloat3 any_perpendicular(cfloat3 n) {
    const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    cfloat3 axis = kernel::cf3(0, 0, 1);
    if (ax <= ay && ax <= az)
        axis = kernel::cf3(1, 0, 0);
    else if (ay <= az)
        axis = kernel::cf3(0, 1, 0);
    return safe_unit(kernel::ccross(axis, n), kernel::cf3(1, 0, 0));
}

// The frame's remaining two axes from a normal and a rough tangent direction.
SurfaceFrame orthonormalize(cfloat3 normal, cfloat3 rough_tangent) {
    SurfaceFrame f;
    f.normal = safe_unit(normal, kernel::cf3(0, 1, 0));
    cfloat3 t = rough_tangent - f.normal * kernel::cdot(rough_tangent, f.normal);
    f.tangent = safe_unit(t, any_perpendicular(f.normal));
    f.bitangent = kernel::ccross(f.normal, f.tangent);
    return f;
}

// The polygon's area vector. Newell's sum rather than a corner cross product
// because a subdivided quad is not planar and a cross product would depend on
// which corner it was taken at; the sum's LENGTH is twice the area, which is
// the weighting a vertex normal wants.
cfloat3 newell(const LevelTopology& topology, const std::vector<cfloat3>& positions,
               std::uint32_t f) {
    std::uint32_t arity = 0;
    const std::uint32_t* c = topology.face(f, &arity);
    cfloat3 n = kernel::cf3(0, 0, 0);
    for (std::uint32_t i = 0; i < arity; ++i) {
        const cfloat3 a = positions[c[i]];
        const cfloat3 b = positions[c[(i + 1) % arity]];
        n = n + kernel::cf3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x),
                            (a.x - b.x) * (a.y + b.y));
    }
    return n;
}

// The mean of several frames' tangents, which is what an edge point and a face
// point inherit. Averaging the TANGENTS rather than picking one keeps the
// result continuous as the parents rotate relative to each other.
struct TangentMean {
    cfloat3 tangent = kernel::cf3(0, 0, 0);
    cfloat3 normal = kernel::cf3(0, 0, 0);
    std::uint32_t count = 0;

    void add(const SurfaceFrame& f) {
        // Sign-aligned against the first contribution, so two parents whose
        // tangents happen to point oppositely do not cancel into nothing.
        const float s = (count == 0 || kernel::cdot(f.tangent, tangent) >= 0.0f) ? 1.0f : -1.0f;
        tangent = tangent + f.tangent * s;
        normal = normal + f.normal;
        ++count;
    }
};

SurfaceFrame child_frame_of(std::uint32_t child, const ChildLayout& layout,
                            const LevelTopology& parent, const LevelConnectivity& conn,
                            const std::vector<SurfaceFrame>& parent_frames,
                            const std::vector<cfloat3>& child_normals) {
    TangentMean mean;
    switch (layout.origin_of(child)) {
        case SubdivisionOrigin::VertexPoint:
            mean.add(parent_frames[child]);
            break;
        case SubdivisionOrigin::EdgePoint: {
            const LevelEdge& e = conn.edges[child - layout.edge_base];
            mean.add(parent_frames[e.a]);
            mean.add(parent_frames[e.b]);
            break;
        }
        default: {
            std::uint32_t arity = 0;
            const std::uint32_t* corners = parent.face(child - layout.face_base, &arity);
            for (std::uint32_t i = 0; i < arity; ++i) mean.add(parent_frames[corners[i]]);
            break;
        }
    }
    const cfloat3 source_normal = safe_unit(mean.normal, child_normals[child]);
    const cfloat3 target_normal = safe_unit(child_normals[child], source_normal);
    const cfloat3 carried = rotate_shortest_arc(mean.tangent, source_normal, target_normal);
    return orthonormalize(target_normal, carried);
}

}  // namespace

cfloat3 rotate_shortest_arc(cfloat3 v, cfloat3 from, cfloat3 to) {
    const cfloat3 axis = kernel::ccross(from, to);
    const float s2 = kernel::cdot(axis, axis);
    const float c = kernel::cdot(from, to);
    // Already aligned: nothing to do, and the general formula would divide by a
    // vanishing sine.
    if (s2 < 1e-16f) {
        if (c >= 0.0f) return v;
        // Antipodal. There is no shortest arc — every half-turn is as short as
        // every other — so the caller's re-orthonormalization is what settles
        // it, and reflecting through the plane is the continuous limit.
        const cfloat3 axis2 = any_perpendicular(from);
        return axis2 * (2.0f * kernel::cdot(v, axis2)) - v;
    }
    const float s = std::sqrt(s2);
    const cfloat3 k = axis / s;
    const float angle = std::atan2(s, c);
    const float ca = std::cos(angle), sa = std::sin(angle);
    return v * ca + kernel::ccross(k, v) * sa + k * (kernel::cdot(k, v) * (1.0f - ca));
}

void level_normals(const LevelTopology& topology, const LevelConnectivity& conn,
                   const std::vector<cfloat3>& positions, std::vector<cfloat3>* out) {
    out->assign(topology.vertex_count, kernel::cf3(0, 0, 0));
    std::vector<cfloat3> face_normal(topology.face_count);
    for (std::uint32_t f = 0; f < topology.face_count; ++f)
        face_normal[f] = newell(topology, positions, f);
    for (std::uint32_t v = 0; v < topology.vertex_count; ++v) {
        std::size_t count = 0;
        const std::uint32_t* faces = conn.faces_of(v, &count);
        cfloat3 sum = kernel::cf3(0, 0, 0);
        for (std::size_t i = 0; i < count; ++i) sum = sum + face_normal[faces[i]];
        (*out)[v] = safe_unit(sum, kernel::cf3(0, 1, 0));
    }
}

void level_normals_partial(const LevelTopology& topology, const LevelConnectivity& conn,
                           const std::vector<cfloat3>& positions,
                           const std::vector<std::uint32_t>& vertices,
                           std::vector<cfloat3>* inout) {
    if (inout->size() < topology.vertex_count) inout->resize(topology.vertex_count, kernel::cf3(0, 1, 0));
    for (std::uint32_t v : vertices) {
        std::size_t count = 0;
        const std::uint32_t* faces = conn.faces_of(v, &count);
        cfloat3 sum = kernel::cf3(0, 0, 0);
        for (std::size_t i = 0; i < count; ++i) sum = sum + newell(topology, positions, faces[i]);
        (*inout)[v] = safe_unit(sum, kernel::cf3(0, 1, 0));
    }
}

// The UV tangent of one face: the world direction in which u increases,
// solved from the face's first three corners. Returns the zero vector when the
// parametrization is degenerate there (a collapsed UV triangle), which the
// caller reads as "this face has nothing to say about the tangent".
namespace {

cfloat3 uv_tangent_of_face(const LevelTopology& topology, const std::vector<cfloat3>& positions,
                           const std::vector<kernel::cfloat2>& uvs, std::uint32_t f) {
    std::uint32_t arity = 0;
    const std::uint32_t* c = topology.face(f, &arity);
    if (arity < 3) return kernel::cf3(0, 0, 0);
    const cfloat3 e1 = positions[c[1]] - positions[c[0]];
    const cfloat3 e2 = positions[c[2]] - positions[c[0]];
    const kernel::cfloat2 d1 = uvs[c[1]] - uvs[c[0]];
    const kernel::cfloat2 d2 = uvs[c[2]] - uvs[c[0]];
    const float r = d1.x * d2.y - d2.x * d1.y;
    if (std::fabs(r) < 1e-20f) return kernel::cf3(0, 0, 0);
    // NOT divided by r: the magnitude is then proportional to the face's own
    // scale, which is the area weighting a per-vertex accumulation wants. Only
    // the SIGN of r matters for the direction.
    const cfloat3 t = e1 * d2.y - e2 * d1.y;
    return r > 0.0f ? t : -t;
}

}  // namespace

namespace {

SurfaceFrame base_frame_of(const LevelTopology& topology, const LevelConnectivity& conn,
                           const std::vector<cfloat3>& positions,
                           const std::vector<cfloat3>& normals,
                           const std::vector<kernel::cfloat2>* uvs, bool use_uv, std::uint32_t v) {
    cfloat3 rough = kernel::cf3(0, 0, 0);
    if (use_uv) {
        std::size_t face_count = 0;
        const std::uint32_t* faces = conn.faces_of(v, &face_count);
        for (std::size_t i = 0; i < face_count; ++i)
            rough = rough + uv_tangent_of_face(topology, positions, *uvs, faces[i]);
    }
    if (kernel::cdot(rough, rough) <= 1e-24f) {
        // The geometric fallback: toward the LOWEST-INDEXED ring neighbour.
        // Chosen by index rather than by distance or angle, so no deformation
        // can change which neighbour it is and no deformation can therefore
        // flip the frame.
        std::size_t ring_count = 0;
        const std::uint32_t* ring = conn.edges_of(v, &ring_count);
        std::uint32_t best = kNoVertex;
        for (std::size_t i = 0; i < ring_count; ++i) {
            const LevelEdge& e = conn.edges[ring[i]];
            const std::uint32_t other = (e.a == v) ? e.b : e.a;
            if (best == kNoVertex || other < best) best = other;
        }
        if (best != kNoVertex) rough = positions[best] - positions[v];
    }
    return orthonormalize(normals[v], rough);
}

}  // namespace

void build_base_frames(const LevelTopology& topology, const LevelConnectivity& conn,
                       const std::vector<cfloat3>& positions, const std::vector<cfloat3>& normals,
                       const std::vector<kernel::cfloat2>* uvs, std::vector<SurfaceFrame>* out) {
    const bool use_uv = uvs != nullptr && uvs->size() == positions.size();
    out->assign(topology.vertex_count, SurfaceFrame{});
    for (std::uint32_t v = 0; v < topology.vertex_count; ++v)
        (*out)[v] = base_frame_of(topology, conn, positions, normals, uvs, use_uv, v);
}

void build_base_frames_partial(const LevelTopology& topology, const LevelConnectivity& conn,
                               const std::vector<cfloat3>& positions,
                               const std::vector<cfloat3>& normals,
                               const std::vector<kernel::cfloat2>* uvs,
                               const std::vector<std::uint32_t>& vertices,
                               std::vector<SurfaceFrame>* inout) {
    const bool use_uv = uvs != nullptr && uvs->size() == positions.size();
    if (inout->size() < topology.vertex_count) inout->resize(topology.vertex_count, SurfaceFrame{});
    for (std::uint32_t v : vertices)
        if (v < topology.vertex_count)
            (*inout)[v] = base_frame_of(topology, conn, positions, normals, uvs, use_uv, v);
}

void transport_frames(const LevelTopology& parent, const LevelConnectivity& conn,
                      const std::vector<SurfaceFrame>& parent_frames,
                      const std::vector<cfloat3>& child_normals, std::vector<SurfaceFrame>* out) {
    const ChildLayout layout = ChildLayout::of(parent, conn);
    out->assign(layout.total, SurfaceFrame{});
    for (std::uint32_t c = 0; c < layout.total; ++c)
        (*out)[c] = child_frame_of(c, layout, parent, conn, parent_frames, child_normals);
}

void transport_frames_partial(const LevelTopology& parent, const LevelConnectivity& conn,
                              const std::vector<SurfaceFrame>& parent_frames,
                              const std::vector<cfloat3>& child_normals,
                              const std::vector<std::uint32_t>& child_vertices,
                              std::vector<SurfaceFrame>* inout) {
    const ChildLayout layout = ChildLayout::of(parent, conn);
    if (inout->size() < layout.total) inout->resize(layout.total, SurfaceFrame{});
    for (std::uint32_t c : child_vertices)
        (*inout)[c] = child_frame_of(c, layout, parent, conn, parent_frames, child_normals);
}

}  // namespace mesh
}  // namespace clay
