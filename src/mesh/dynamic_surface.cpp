#include "clay/mesh/dynamic_surface.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace clay {
namespace mesh {

namespace {

kernel::cfloat3 safe_unit(kernel::cfloat3 v, kernel::cfloat3 fallback) {
    const float len = kernel::clength(v);
    return len > 1e-20f ? v / len : fallback;
}

// The interior angle a triangle subtends at one of its corners. Same function
// the fixed sculptor uses, and for the same reason: the angle is a property of
// the surface rather than of how it was triangulated.
float corner_angle(kernel::cfloat3 p, kernel::cfloat3 a, kernel::cfloat3 b) {
    const kernel::cfloat3 u = a - p, v = b - p;
    const float lu = kernel::clength(u), lv = kernel::clength(v);
    if (lu < 1e-20f || lv < 1e-20f) return 0.0f;
    return std::acos(std::clamp(kernel::cdot(u, v) / (lu * lv), -1.0f, 1.0f));
}

// A quantized position, for welding. The same lattice trick `Adjacency` uses:
// coincident-within-epsilon vertices land in the same cell, and the 27
// neighbouring cells are searched so a pair straddling a cell boundary is still
// found.
struct CellKey {
    std::int64_t x, y, z;
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellHash {
    std::size_t operator()(const CellKey& k) const {
        std::uint64_t h = 1469598103934665603ull;
        for (std::int64_t v : {k.x, k.y, k.z}) {
            h ^= static_cast<std::uint64_t>(v);
            h *= 1099511628211ull;
        }
        return static_cast<std::size_t>(h);
    }
};

CellKey cell_of(kernel::cfloat3 p, float eps) {
    const float inv = 1.0f / std::max(eps, 1e-20f);
    return CellKey{static_cast<std::int64_t>(std::floor(p.x * inv)),
                   static_cast<std::int64_t>(std::floor(p.y * inv)),
                   static_cast<std::int64_t>(std::floor(p.z * inv))};
}

}  // namespace

// -- traversal ----------------------------------------------------------------

VertexId DynamicSurface::origin_of(HalfEdgeId h) const {
    const DynamicHalfEdge* he = halfedges_.get(h);
    return he ? he->origin : VertexId{};
}

VertexId DynamicSurface::target_of(HalfEdgeId h) const {
    const DynamicHalfEdge* he = halfedges_.get(h);
    if (!he) return VertexId{};
    const DynamicHalfEdge* tw = halfedges_.get(he->twin);
    return tw ? tw->origin : VertexId{};
}

HalfEdgeId DynamicSurface::twin_of(HalfEdgeId h) const {
    const DynamicHalfEdge* he = halfedges_.get(h);
    return he ? he->twin : HalfEdgeId{};
}

HalfEdgeId DynamicSurface::next_of(HalfEdgeId h) const {
    const DynamicHalfEdge* he = halfedges_.get(h);
    return he ? he->next : HalfEdgeId{};
}

HalfEdgeId DynamicSurface::prev_of(HalfEdgeId h) const {
    // A face is a triangle, so the previous corner is two steps on. Written as
    // a walk rather than assumed, so a face that is momentarily malformed
    // mid-operator returns an invalid handle instead of a plausible wrong one.
    const HalfEdgeId n = next_of(h);
    return next_of(n);
}

EdgeId DynamicSurface::edge_of(HalfEdgeId h) const {
    const DynamicHalfEdge* he = halfedges_.get(h);
    return he ? he->edge : EdgeId{};
}

FaceId DynamicSurface::face_of(HalfEdgeId h) const {
    const DynamicHalfEdge* he = halfedges_.get(h);
    return he ? he->face : FaceId{};
}

HalfEdgeId DynamicSurface::halfedge_of(EdgeId e) const {
    const DynamicEdge* ed = edges_.get(e);
    return ed ? ed->halfedge : HalfEdgeId{};
}

bool DynamicSurface::is_boundary_halfedge(HalfEdgeId h) const {
    const DynamicHalfEdge* he = halfedges_.get(h);
    return he && !faces_.live(he->face);
}

bool DynamicSurface::is_boundary_edge(EdgeId e) const {
    const HalfEdgeId h = halfedge_of(e);
    return is_boundary_halfedge(h) || is_boundary_halfedge(twin_of(h));
}

bool DynamicSurface::is_boundary_vertex(VertexId v) const {
    const DynamicVertex* vert = vertices_.get(v);
    if (!vert) return false;
    // `refresh_outgoing` prefers a boundary half-edge, so the common case is
    // one comparison. The walk is the fallback for a vertex whose outgoing was
    // set by an operator that had no reason to prefer one.
    if (is_boundary_halfedge(vert->outgoing)) return true;
    std::vector<HalfEdgeId> ring;
    if (!outgoing_halfedges(v, &ring)) return true;  // an open ring IS a border
    for (HalfEdgeId h : ring)
        if (is_boundary_halfedge(h)) return true;
    return false;
}

kernel::cfloat3 DynamicSurface::position_of(VertexId v) const {
    const DynamicVertex* vert = vertices_.get(v);
    return vert ? vert->position : kernel::cf3(0, 0, 0);
}

float DynamicSurface::edge_length(EdgeId e) const {
    const HalfEdgeId h = halfedge_of(e);
    return kernel::clength(position_of(target_of(h)) - position_of(origin_of(h)));
}

kernel::cfloat3 DynamicSurface::edge_midpoint(EdgeId e) const {
    const HalfEdgeId h = halfedge_of(e);
    return (position_of(origin_of(h)) + position_of(target_of(h))) * 0.5f;
}

// The outgoing ring, walked as twin-then-next.
//
// WHY THAT COMBINATION and not next-then-twin: `twin(h)` points back AT `v`
// along the same edge, and `next` of that half-edge leaves `v` again along the
// following edge. It walks the ring in one direction and terminates on the
// start — or hits a boundary, where it has to restart from the other side,
// which is what the second loop does.
bool DynamicSurface::outgoing_halfedges(VertexId v, std::vector<HalfEdgeId>* out) const {
    out->clear();
    const DynamicVertex* vert = vertices_.get(v);
    if (!vert || !halfedges_.live(vert->outgoing)) return false;

    const HalfEdgeId start = vert->outgoing;
    HalfEdgeId h = start;
    // A bound rather than a trust: a corrupted ring must terminate the walk
    // instead of hanging the process, and the validator is what reports it.
    const std::size_t bound = halfedges_.capacity_slots() + 1;
    bool closed = false;
    for (std::size_t guard = 0; guard < bound; ++guard) {
        out->push_back(h);
        const HalfEdgeId tw = twin_of(h);
        if (!halfedges_.live(tw)) return false;
        const HalfEdgeId nx = next_of(tw);
        if (!halfedges_.live(nx)) break;  // boundary: the fan is open
        if (nx == start) {
            closed = true;
            break;
        }
        h = nx;
    }
    if (closed) return true;

    // An open fan: walk the other way from the start and prepend, so the caller
    // gets the whole fan in order rather than half of it.
    std::vector<HalfEdgeId> before;
    h = start;
    for (std::size_t guard = 0; guard < bound; ++guard) {
        const HalfEdgeId pv = prev_of(h);
        if (!halfedges_.live(pv)) break;
        const HalfEdgeId tw = twin_of(pv);
        if (!halfedges_.live(tw) || tw == start) break;
        before.push_back(tw);
        h = tw;
    }
    if (!before.empty()) {
        std::reverse(before.begin(), before.end());
        before.insert(before.end(), out->begin(), out->end());
        out->swap(before);
    }
    return true;
}

bool DynamicSurface::one_ring(VertexId v, std::vector<VertexId>* out) const {
    out->clear();
    std::vector<HalfEdgeId> ring;
    if (!outgoing_halfedges(v, &ring)) return false;
    out->reserve(ring.size());
    for (HalfEdgeId h : ring) {
        const VertexId t = target_of(h);
        if (t.valid()) out->push_back(t);
    }
    return true;
}

bool DynamicSurface::incident_faces(VertexId v, std::vector<FaceId>* out) const {
    out->clear();
    std::vector<HalfEdgeId> ring;
    if (!outgoing_halfedges(v, &ring)) return false;
    for (HalfEdgeId h : ring) {
        const FaceId f = face_of(h);
        if (faces_.live(f)) out->push_back(f);
    }
    return true;
}

bool DynamicSurface::face_vertices(FaceId f, VertexId out[3]) const {
    const DynamicFace* face_rec = faces_.get(f);
    if (!face_rec) return false;
    HalfEdgeId h = face_rec->halfedge;
    for (int i = 0; i < 3; ++i) {
        if (!halfedges_.live(h)) return false;
        out[i] = origin_of(h);
        if (!out[i].valid()) return false;
        h = next_of(h);
    }
    return h == face_rec->halfedge;  // the loop must close in exactly three
}

std::size_t DynamicSurface::valence(VertexId v) const {
    std::vector<HalfEdgeId> ring;
    if (!outgoing_halfedges(v, &ring)) return 0;
    return ring.size();
}

// -- geometry -----------------------------------------------------------------

kernel::cfloat3 DynamicSurface::face_normal(FaceId f) const {
    VertexId v[3];
    if (!face_vertices(f, v)) return kernel::cf3(0, 1, 0);
    const kernel::cfloat3 a = position_of(v[0]), b = position_of(v[1]), c = position_of(v[2]);
    return safe_unit(kernel::ccross(b - a, c - a), kernel::cf3(0, 1, 0));
}

float DynamicSurface::face_area_x2(FaceId f) const {
    VertexId v[3];
    if (!face_vertices(f, v)) return 0.0f;
    const kernel::cfloat3 a = position_of(v[0]), b = position_of(v[1]), c = position_of(v[2]);
    return kernel::clength(kernel::ccross(b - a, c - a));
}

kernel::cfloat3 DynamicSurface::compute_vertex_normal(VertexId v) const {
    std::vector<HalfEdgeId> ring;
    if (!outgoing_halfedges(v, &ring)) return kernel::cf3(0, 1, 0);
    const kernel::cfloat3 p = position_of(v);
    kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
    for (HalfEdgeId h : ring) {
        const FaceId f = face_of(h);
        if (!faces_.live(f)) continue;
        // The two other corners of this face, from this corner.
        const HalfEdgeId n = next_of(h);
        const HalfEdgeId nn = next_of(n);
        if (!halfedges_.live(n) || !halfedges_.live(nn)) continue;
        const kernel::cfloat3 a = position_of(origin_of(n));
        const kernel::cfloat3 b = position_of(origin_of(nn));
        sum = sum + face_normal(f) * corner_angle(p, a, b);
    }
    return safe_unit(sum, kernel::cf3(0, 1, 0));
}

void DynamicSurface::refresh_normals(const std::vector<FaceId>& faces) {
    // Faces first, then every vertex those faces touch: a vertex normal is an
    // average over its faces, so the faces have to be right before the vertices
    // read them.
    for (FaceId f : faces)
        if (DynamicFace* rec = faces_.get(f)) rec->normal = face_normal(f);

    std::vector<VertexId> touched;
    for (FaceId f : faces) {
        VertexId v[3];
        if (!face_vertices(f, v)) continue;
        for (int i = 0; i < 3; ++i) touched.push_back(v[i]);
    }
    // Sorted and uniqued by slot, which both removes the duplicates a shared
    // vertex creates and gives the pass a deterministic order.
    std::sort(touched.begin(), touched.end(),
              [](VertexId a, VertexId b) { return a.slot < b.slot; });
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (VertexId v : touched)
        if (DynamicVertex* rec = vertices_.get(v)) rec->normal = compute_vertex_normal(v);
}

void DynamicSurface::refresh_all_normals() {
    faces_.for_each_live_mutable(
        [&](FaceId id, DynamicFace& f) { f.normal = face_normal(id); });
    vertices_.for_each_live_mutable(
        [&](VertexId id, DynamicVertex& v) { v.normal = compute_vertex_normal(id); });
}

// -- mutation -----------------------------------------------------------------

VertexId DynamicSurface::create_vertex(const DynamicVertex& v) { return vertices_.create(v); }
HalfEdgeId DynamicSurface::create_halfedge(const DynamicHalfEdge& h) {
    return halfedges_.create(h);
}
EdgeId DynamicSurface::create_edge(const DynamicEdge& e) { return edges_.create(e); }
FaceId DynamicSurface::create_face(const DynamicFace& f) { return faces_.create(f); }

bool DynamicSurface::erase_vertex(VertexId id) { return vertices_.erase(id); }
bool DynamicSurface::erase_halfedge(HalfEdgeId id) { return halfedges_.erase(id); }
bool DynamicSurface::erase_edge(EdgeId id) { return edges_.erase(id); }
bool DynamicSurface::erase_face(FaceId id) { return faces_.erase(id); }

void DynamicSurface::bind_face(FaceId f, HalfEdgeId a, HalfEdgeId b, HalfEdgeId c) {
    DynamicHalfEdge* ha = halfedges_.get(a);
    DynamicHalfEdge* hb = halfedges_.get(b);
    DynamicHalfEdge* hc = halfedges_.get(c);
    if (!ha || !hb || !hc) return;
    ha->next = b;
    hb->next = c;
    hc->next = a;
    ha->face = f;
    hb->face = f;
    hc->face = f;
    if (DynamicFace* rec = faces_.get(f)) rec->halfedge = a;
}

void DynamicSurface::bind_edge(EdgeId e, HalfEdgeId a, HalfEdgeId b) {
    DynamicHalfEdge* ha = halfedges_.get(a);
    DynamicHalfEdge* hb = halfedges_.get(b);
    if (!ha || !hb) return;
    ha->twin = b;
    hb->twin = a;
    ha->edge = e;
    hb->edge = e;
    if (DynamicEdge* rec = edges_.get(e)) rec->halfedge = a;
}

void DynamicSurface::refresh_outgoing(VertexId v) {
    DynamicVertex* vert = vertices_.get(v);
    if (!vert) return;
    std::vector<HalfEdgeId> ring;
    if (!outgoing_halfedges(v, &ring) || ring.empty()) {
        // The walk failed, which happens mid-operator while the ring is being
        // rebuilt. Leave whatever is there; the operator sets it explicitly.
        return;
    }
    // PREFER A BOUNDARY half-edge, so that `is_boundary_vertex` answers from
    // the seed rather than walking, and so that the open fan's walk starts at
    // its beginning rather than in its middle.
    for (HalfEdgeId h : ring)
        if (is_boundary_halfedge(h)) {
            vert->outgoing = h;
            return;
        }
    vert->outgoing = ring.front();
}

DynamicSurfaceStats DynamicSurface::stats() const {
    DynamicSurfaceStats s;
    s.vertices = vertices_.size();
    s.edges = edges_.size();
    s.halfedges = halfedges_.size();
    s.faces = faces_.size();
    edges_.for_each_live([&](EdgeId id, const DynamicEdge&) {
        if (is_boundary_edge(id)) ++s.boundary_edges;
    });
    s.dead_slots = vertices_.dead_slots() + halfedges_.dead_slots() + edges_.dead_slots() +
                   faces_.dead_slots();
    return s;
}

std::size_t DynamicSurface::bytes() const {
    return vertices_.bytes() + halfedges_.bytes() + edges_.bytes() + faces_.bytes();
}

// -- from_mesh ----------------------------------------------------------------

std::optional<DynamicSurface> DynamicSurface::from_mesh(const Mesh& mesh,
                                                        const DynamicSurfaceBuildOptions& options,
                                                        DynamicBuildError* out_error) {
    auto fail = [&](DynamicBuildError e) -> std::optional<DynamicSurface> {
        if (out_error) *out_error = e;
        return std::nullopt;
    };
    if (out_error) *out_error = DynamicBuildError::None;

    if (mesh.positions.empty() || mesh.indices.size() < 3 || mesh.indices.size() % 3 != 0)
        return fail(DynamicBuildError::EmptyMesh);
    for (std::uint32_t i : mesh.indices)
        if (i >= mesh.positions.size()) return fail(DynamicBuildError::IndexOutOfRange);

    const bool has_normals = mesh.normals.size() == mesh.positions.size();
    const bool has_colors = mesh.colors.size() == mesh.positions.size();
    const bool has_uvs = mesh.uvs.size() == mesh.positions.size();

    // -- weld ----------------------------------------------------------------
    //
    // Position-coincident raw vertices become ONE geometric vertex. That is the
    // whole point of the corner domain: an exported seam is a duplicated
    // position carrying two UVs, and welding it here is what lets the seam be
    // an EDGE PROPERTY rather than a hole in the surface.
    const float eps = std::max(options.weld_epsilon, 0.0f);
    std::vector<std::uint32_t> weld_of(mesh.positions.size(), 0xffffffffu);
    std::vector<std::uint32_t> representative;  // weld class -> a raw vertex
    if (eps <= 0.0f) {
        // Exact: a hash on the bits. Faster, and what a caller asking for zero
        // epsilon means.
        std::unordered_map<CellKey, std::uint32_t, CellHash> exact;
        for (std::uint32_t i = 0; i < mesh.positions.size(); ++i) {
            const kernel::cfloat3 p = mesh.positions[i];
            CellKey k{};
            std::memcpy(&k.x, &p.x, 4);
            std::memcpy(&k.y, &p.y, 4);
            std::memcpy(&k.z, &p.z, 4);
            auto it = exact.find(k);
            if (it == exact.end()) {
                weld_of[i] = static_cast<std::uint32_t>(representative.size());
                exact.emplace(k, weld_of[i]);
                representative.push_back(i);
            } else {
                weld_of[i] = it->second;
            }
        }
    } else {
        std::unordered_map<CellKey, std::vector<std::uint32_t>, CellHash> buckets;
        buckets.reserve(mesh.positions.size());
        const float eps2 = eps * eps;
        for (std::uint32_t i = 0; i < mesh.positions.size(); ++i) {
            const kernel::cfloat3 p = mesh.positions[i];
            const CellKey base = cell_of(p, eps);
            std::uint32_t found = 0xffffffffu;
            for (std::int64_t dz = -1; dz <= 1 && found == 0xffffffffu; ++dz)
                for (std::int64_t dy = -1; dy <= 1 && found == 0xffffffffu; ++dy)
                    for (std::int64_t dx = -1; dx <= 1 && found == 0xffffffffu; ++dx) {
                        auto it = buckets.find(CellKey{base.x + dx, base.y + dy, base.z + dz});
                        if (it == buckets.end()) continue;
                        for (std::uint32_t candidate : it->second)
                            if (kernel::cdot2(mesh.positions[candidate] - p) <= eps2) {
                                found = weld_of[candidate];
                                break;
                            }
                    }
            if (found == 0xffffffffu) {
                found = static_cast<std::uint32_t>(representative.size());
                representative.push_back(i);
            }
            weld_of[i] = found;
            buckets[base].push_back(i);
        }
    }

    DynamicSurface surface;
    surface.set_source_attributes(has_normals, has_colors, has_uvs);
    surface.vertices_.reserve(representative.size());

    std::vector<VertexId> vertex_of_class(representative.size());
    for (std::size_t c = 0; c < representative.size(); ++c) {
        const std::uint32_t raw = representative[c];
        DynamicVertex v;
        v.position = mesh.positions[raw];
        if (has_normals) v.normal = mesh.normals[raw];
        if (has_colors) v.color = mesh.colors[raw];
        vertex_of_class[c] = surface.vertices_.create(v);
    }

    // -- faces and half-edges -------------------------------------------------
    const std::size_t triangles = mesh.indices.size() / 3;
    surface.halfedges_.reserve(triangles * 3);
    surface.faces_.reserve(triangles);
    surface.edges_.reserve(triangles * 2);

    // Directed corner -> half-edge, so a twin is found by looking up the
    // reversed pair. A THIRD sighting of the same directed pair is a
    // non-manifold edge, which this representation cannot express.
    struct PairKey {
        std::uint32_t a, b;
        bool operator==(const PairKey& o) const { return a == o.a && b == o.b; }
    };
    struct PairHash {
        std::size_t operator()(const PairKey& k) const {
            return static_cast<std::size_t>(k.a) * 0x9E3779B97F4A7C15ull + k.b;
        }
    };
    std::unordered_map<PairKey, HalfEdgeId, PairHash> directed;
    directed.reserve(triangles * 3);

    for (std::size_t t = 0; t < triangles; ++t) {
        const std::uint32_t raw[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1],
                                      mesh.indices[t * 3 + 2]};
        const std::uint32_t cls[3] = {weld_of[raw[0]], weld_of[raw[1]], weld_of[raw[2]]};
        // A triangle two of whose corners welded together has no area and no
        // meaningful normal, and every operator downstream would have to guard
        // against it. Refused at the boundary instead.
        if (cls[0] == cls[1] || cls[1] == cls[2] || cls[0] == cls[2])
            return fail(DynamicBuildError::DegenerateTriangle);

        const FaceId f = surface.faces_.create(DynamicFace{});
        HalfEdgeId h[3];
        for (int i = 0; i < 3; ++i) {
            DynamicHalfEdge he;
            he.origin = vertex_of_class[cls[i]];
            he.face = f;
            // CORNER UV, taken from the RAW vertex rather than the welded one.
            // This is where a seam survives: two corners at one geometric
            // vertex keep the two different UVs their raw vertices carried.
            if (has_uvs) he.uv = mesh.uvs[raw[i]];
            h[i] = surface.halfedges_.create(he);
            // A FIRST outgoing, set here rather than left to
            // `refresh_outgoing`: that walks the ring, and the ring walk starts
            // from the outgoing it is trying to establish. Something has to
            // break the cycle, and creation is the only place that can.
            DynamicVertex& vert = surface.vertices_.at(he.origin);
            if (!vert.outgoing.valid()) vert.outgoing = h[i];
        }
        surface.bind_face(f, h[0], h[1], h[2]);

        for (int i = 0; i < 3; ++i) {
            const std::uint32_t from = cls[i], to = cls[(i + 1) % 3];
            if (directed.find(PairKey{from, to}) != directed.end())
                return fail(DynamicBuildError::NonManifoldEdge);
            directed.emplace(PairKey{from, to}, h[i]);
        }
    }

    // -- pair the twins -------------------------------------------------------
    //
    // Walked in SLOT ORDER rather than over the hash map, so the edges and the
    // boundary half-edges come out in the same order on every platform and
    // every standard library. A surface whose element numbering depended on
    // hash iteration would make every later "same input, same output" claim
    // false in a way no test would notice.
    std::vector<HalfEdgeId> unpaired;
    surface.halfedges_.for_each_live([&](HalfEdgeId id, const DynamicHalfEdge&) {
        if (!surface.halfedges_.at(id).twin.valid()) unpaired.push_back(id);
    });

    for (HalfEdgeId id : unpaired) {
        DynamicHalfEdge& he = surface.halfedges_.at(id);
        if (he.twin.valid()) continue;  // paired by an earlier sighting
        const VertexId from = he.origin;
        const VertexId to = surface.origin_of(surface.next_of(id));
        auto it = directed.find(PairKey{to.slot, from.slot});
        const EdgeId e = surface.edges_.create(DynamicEdge{});
        if (it != directed.end()) {
            surface.bind_edge(e, id, it->second);
        } else {
            // An open border. A boundary half-edge with no face is created so
            // that every half-edge has a twin and the traversal never has to
            // special-case a null one — the standard construction, and the
            // reason `face` is optional rather than `twin`.
            DynamicHalfEdge ghost;
            ghost.origin = to;
            ghost.uv = surface.halfedges_.at(surface.next_of(id)).uv;
            const HalfEdgeId gid = surface.halfedges_.create(ghost);
            surface.bind_edge(e, id, gid);
            // A boundary half-edge is the PREFERRED seed, so `refresh_outgoing`
            // and `is_boundary_vertex` answer from the seed rather than walking.
            surface.vertices_.at(ghost.origin).outgoing = gid;
            surface.edges_.at(e).constraints |= EdgeConstraint::Boundary;
        }
    }

    // -- link the boundary loops ---------------------------------------------
    //
    // A boundary half-edge's `next` is the next boundary half-edge around the
    // hole, which is what makes the outgoing walk terminate on an open fan
    // rather than falling off the surface.
    std::vector<HalfEdgeId> boundary;
    surface.halfedges_.for_each_live([&](HalfEdgeId id, const DynamicHalfEdge& he) {
        if (!surface.faces_.live(he.face)) boundary.push_back(id);
    });
    for (HalfEdgeId id : boundary) {
        // From this ghost's origin, find the outgoing boundary half-edge: walk
        // the fan around that vertex the other way until the twin has no face.
        const VertexId v = surface.halfedges_.at(id).origin;
        HalfEdgeId scan = surface.twin_of(id);
        const std::size_t bound = surface.halfedges_.capacity_slots() + 1;
        for (std::size_t guard = 0; guard < bound; ++guard) {
            const HalfEdgeId prev = surface.prev_of(scan);
            if (!surface.halfedges_.live(prev)) break;
            const HalfEdgeId tw = surface.twin_of(prev);
            if (!surface.halfedges_.live(tw)) break;
            if (!surface.faces_.live(surface.halfedges_.at(tw).face)) {
                surface.halfedges_.at(id).next = tw;
                break;
            }
            scan = tw;
        }
        (void)v;
    }

    // -- outgoing seeds, seams, normals --------------------------------------
    surface.vertices_.for_each_live_mutable(
        [&](VertexId id, DynamicVertex&) { surface.refresh_outgoing(id); });

    // A UV SEAM is an edge whose two sides disagree about the UV at a shared
    // vertex. Detected here rather than trusted from the file, because a mesh
    // that welded cleanly may still carry a seam and nothing in a triangle list
    // says which edges are on it.
    if (has_uvs) {
        const float uv_eps = std::max(options.uv_seam_epsilon, 0.0f);
        surface.edges_.for_each_live_mutable([&](EdgeId, DynamicEdge& e) {
            const HalfEdgeId a = e.halfedge;
            const HalfEdgeId b = surface.twin_of(a);
            if (!surface.halfedges_.live(b)) return;
            if (surface.is_boundary_halfedge(a) || surface.is_boundary_halfedge(b)) return;
            // The two corners at each end of the edge, one from each side.
            const kernel::cfloat2 a0 = surface.halfedges_.at(a).uv;
            const kernel::cfloat2 a1 = surface.halfedges_.at(surface.next_of(a)).uv;
            const kernel::cfloat2 b0 = surface.halfedges_.at(b).uv;
            const kernel::cfloat2 b1 = surface.halfedges_.at(surface.next_of(b)).uv;
            // `a` runs from its origin to `b`'s origin, so the pairs that must
            // agree are (a0, b1) and (a1, b0).
            const float d0 = std::fabs(a0.x - b1.x) + std::fabs(a0.y - b1.y);
            const float d1 = std::fabs(a1.x - b0.x) + std::fabs(a1.y - b0.y);
            if (d0 > uv_eps || d1 > uv_eps) e.constraints |= EdgeConstraint::UvSeam;
        });
    }

    if (!has_normals) surface.refresh_all_normals();
    return surface;
}

// -- to_mesh ------------------------------------------------------------------

Mesh DynamicSurface::to_mesh(const DynamicSurfaceExportOptions& options) const {
    Mesh out;
    const bool want_normals = options.normals && had_normals_;
    const bool want_colors = options.colors && had_colors_;
    const bool want_uvs = options.uvs && had_uvs_;

    // ONE EXPORT VERTEX PER DISTINCT CORNER ATTRIBUTE SET, which is how a flat
    // mesh represents a seam. A geometric vertex whose corners all agree emits
    // one; one on a seam emits as many as it has distinct UVs — the duplicates
    // the importer welded, put back.
    struct Corner {
        kernel::cfloat2 uv;
        std::uint32_t index;
    };
    std::unordered_map<std::uint32_t, std::vector<Corner>> emitted;  // vertex slot -> corners
    emitted.reserve(vertices_.size());

    // Faces in SLOT ORDER, so the index buffer is a function of the surface and
    // not of the order faces happened to be created in.
    std::vector<FaceId> ordered;
    ordered.reserve(faces_.size());
    faces_.for_each_live([&](FaceId id, const DynamicFace&) { ordered.push_back(id); });

    auto emit = [&](VertexId v, kernel::cfloat2 uv) -> std::uint32_t {
        std::vector<Corner>& list = emitted[v.slot];
        if (want_uvs) {
            for (const Corner& c : list)
                if (c.uv.x == uv.x && c.uv.y == uv.y) return c.index;
        } else if (!list.empty()) {
            return list.front().index;
        }
        const DynamicVertex& rec = vertices_.at(v);
        const std::uint32_t index = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(rec.position);
        if (want_normals) out.normals.push_back(rec.normal);
        if (want_colors) out.colors.push_back(rec.color);
        if (want_uvs) out.uvs.push_back(uv);
        list.push_back(Corner{uv, index});
        return index;
    };

    out.indices.reserve(ordered.size() * 3);
    for (FaceId f : ordered) {
        const DynamicFace& rec = faces_.at(f);
        HalfEdgeId h = rec.halfedge;
        std::uint32_t corner[3];
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            if (!halfedges_.live(h)) {
                ok = false;
                break;
            }
            const DynamicHalfEdge& he = halfedges_.at(h);
            if (!vertices_.live(he.origin)) {
                ok = false;
                break;
            }
            corner[i] = emit(he.origin, he.uv);
            h = he.next;
        }
        if (!ok) continue;
        out.indices.push_back(corner[0]);
        out.indices.push_back(corner[1]);
        out.indices.push_back(corner[2]);
    }

    // TRIANGLES, AND THE EXPORT SAYS SO. `quads` is empty and none is
    // re-derived; see D11. A caller who wants quads retopologises.
    out.quads.clear();
    return out;
}

}  // namespace mesh
}  // namespace clay
