#include "clay/mesh/topology_ops.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace clay {
namespace mesh {
namespace {

kernel::cfloat3 lerp3(kernel::cfloat3 a, kernel::cfloat3 b, float t) {
    // Exact at both ends, for the reason `blend_color` already records:
    // a + (b - a) * 1 is not b in floating point, and a split at t = 1 landing
    // one ulp off its own endpoint is a crack.
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (b - a) * t;
}

kernel::cfloat2 lerp2(kernel::cfloat2 a, kernel::cfloat2 b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return kernel::cf2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

float lerp1(float a, float b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (b - a) * t;
}

// The smallest interior angle of a triangle, in radians. Zero for a sliver,
// pi/3 for an equilateral one — the quality metric a flip compares.
float min_angle(kernel::cfloat3 a, kernel::cfloat3 b, kernel::cfloat3 c) {
    auto angle_at = [](kernel::cfloat3 p, kernel::cfloat3 q, kernel::cfloat3 r) {
        const kernel::cfloat3 u = q - p, v = r - p;
        const float lu = kernel::clength(u), lv = kernel::clength(v);
        if (lu < 1e-20f || lv < 1e-20f) return 0.0f;
        return std::acos(std::clamp(kernel::cdot(u, v) / (lu * lv), -1.0f, 1.0f));
    };
    return std::min({angle_at(a, b, c), angle_at(b, c, a), angle_at(c, a, b)});
}

float triangle_area_x2(kernel::cfloat3 a, kernel::cfloat3 b, kernel::cfloat3 c) {
    return kernel::clength(kernel::ccross(b - a, c - a));
}

kernel::cfloat3 triangle_normal(kernel::cfloat3 a, kernel::cfloat3 b, kernel::cfloat3 c) {
    const kernel::cfloat3 n = kernel::ccross(b - a, c - a);
    const float len = kernel::clength(n);
    return len > 1e-20f ? n / len : kernel::cf3(0, 0, 0);
}

// Note an element and everything the operator is about to rewire around it.
// Collected in one place so an operator cannot forget half of what it touched —
// a delta missing one half-edge reverts to a surface that is almost right,
// which is worse than one that is obviously wrong.
struct DeltaScribe {
    DynamicSurface& surface;
    TopologyDelta* delta;
    // WHAT WAS TOUCHED, so the closing sync costs the neighbourhood rather than
    // the surface. The first draft synced by sweeping every live half-edge and
    // edge, which is correct and is O(surface) per operator — the exact thing
    // "a dab costs what it touches" forbids, hidden inside the undo path where
    // no timing test would look for it.
    std::vector<VertexId> vs;
    std::vector<HalfEdgeId> hs;
    std::vector<EdgeId> es;
    std::vector<FaceId> fs;

    void note(VertexId id) {
        if (!id.valid()) return;
        vs.push_back(id);
        if (delta) delta->note_vertex(surface, id);
    }
    void note(HalfEdgeId id) {
        if (!id.valid()) return;
        hs.push_back(id);
        if (delta) delta->note_halfedge(surface, id);
    }
    void note(EdgeId id) {
        if (!id.valid()) return;
        es.push_back(id);
        if (delta) delta->note_edge(surface, id);
    }
    void note(FaceId id) {
        if (!id.valid()) return;
        fs.push_back(id);
        if (delta) delta->note_face(surface, id);
    }

    // An element the operator has just CREATED. Recorded as not having existed,
    // which `note` cannot do: it reads liveness from the pool, and a freshly
    // created element is live.
    void note_new(VertexId id) {
        if (!id.valid()) return;
        vs.push_back(id);
        if (delta) delta->note_new_vertex(id);
    }
    void note_new(HalfEdgeId id) {
        if (!id.valid()) return;
        hs.push_back(id);
        if (delta) delta->note_new_halfedge(id);
    }
    void note_new(EdgeId id) {
        if (!id.valid()) return;
        es.push_back(id);
        if (delta) delta->note_new_edge(id);
    }
    void note_new(FaceId id) {
        if (!id.valid()) return;
        fs.push_back(id);
        if (delta) delta->note_new_face(id);
    }

    // Sync everything this operator touched, once, at the end.
    void sync_all() {
        if (!delta) return;
        for (VertexId id : vs) delta->sync_vertex(surface, id);
        for (HalfEdgeId id : hs) delta->sync_halfedge(surface, id);
        for (EdgeId id : es) delta->sync_edge(surface, id);
        for (FaceId id : fs) delta->sync_face(surface, id);
    }
    void sync(VertexId id) {
        if (delta) delta->sync_vertex(surface, id);
    }
    void sync(HalfEdgeId id) {
        if (delta) delta->sync_halfedge(surface, id);
    }
    void sync(EdgeId id) {
        if (delta) delta->sync_edge(surface, id);
    }
    void sync(FaceId id) {
        if (delta) delta->sync_face(surface, id);
    }
};

// The boundary half-edge whose `next` is `ghost`.
//
// Found through the FAN of the ghost's origin rather than by walking the border
// loop, so it costs the vertex's valence rather than the length of the border —
// which on a big patch is the difference between local and not.
HalfEdgeId boundary_predecessor(const DynamicSurface& s, HalfEdgeId ghost) {
    const VertexId v = s.origin_of(ghost);
    std::vector<HalfEdgeId> ring;
    if (!s.outgoing_halfedges(v, &ring)) return HalfEdgeId{};
    for (HalfEdgeId h : ring) {
        const HalfEdgeId t = s.twin_of(h);
        if (s.live(t) && s.is_boundary_halfedge(t) && s.next_of(t) == ghost) return t;
    }
    return HalfEdgeId{};
}

// Point `v` at a live half-edge that actually starts there, choosing from
// `candidates`.
//
// WHY A CANDIDATE LIST rather than `refresh_outgoing`. That walks the ring, and
// the ring walk starts from the outgoing it is trying to repair — so it cannot
// fix a vertex whose outgoing an operator just erased. The operators know which
// half-edges they touched, and the surviving ones are exactly where to look.
// Bounded by the two-ring, which is the neighbourhood an operator rewired
// anyway.
void reseat_outgoing(DynamicSurface& s, VertexId v, const std::vector<HalfEdgeId>& candidates) {
    DynamicVertex* rec = s.vertex(v);
    if (!rec) return;
    if (s.live(rec->outgoing) && s.origin_of(rec->outgoing) == v &&
        !s.is_boundary_halfedge(rec->outgoing)) {
        // Live and correct, but a boundary seed is still preferred — see
        // `refresh_outgoing`.
        for (HalfEdgeId h : candidates)
            if (s.live(h) && s.origin_of(h) == v && s.is_boundary_halfedge(h)) {
                rec->outgoing = h;
                return;
            }
        return;
    }
    HalfEdgeId fallback;
    for (HalfEdgeId h : candidates) {
        if (!s.live(h) || !(s.origin_of(h) == v)) continue;
        if (s.is_boundary_halfedge(h)) {
            rec->outgoing = h;
            return;
        }
        if (!fallback.valid()) fallback = h;
    }
    if (fallback.valid()) rec->outgoing = fallback;
}

}  // namespace

float edge_pair_quality(const DynamicSurface& surface, EdgeId edge) {
    const HalfEdgeId a = surface.halfedge_of(edge);
    const HalfEdgeId b = surface.twin_of(a);
    const FaceId fa = surface.face_of(a);
    const FaceId fb = surface.face_of(b);
    float worst = 3.15f;  // more than pi, so any real triangle lowers it
    for (FaceId f : {fa, fb}) {
        if (!surface.live(f)) continue;
        VertexId v[3];
        if (!surface.face_vertices(f, v)) return 0.0f;
        worst = std::min(worst, min_angle(surface.position_of(v[0]), surface.position_of(v[1]),
                                          surface.position_of(v[2])));
    }
    return worst;
}

// -- split --------------------------------------------------------------------

SplitResult split_edge(DynamicSurface& surface, EdgeId edge, float t,
                       const TopologyOpOptions& options, TopologyDelta* delta) {
    SplitResult out;
    if (!surface.live(edge)) return out;

    const HalfEdgeId h0 = surface.halfedge_of(edge);
    const HalfEdgeId h1 = surface.twin_of(h0);
    if (!surface.live(h0) || !surface.live(h1)) return out;

    // A split is refused on a locked edge and on nothing else. A boundary
    // splits (it stays a boundary), a seam splits (both sides interpolate their
    // own UV), and a crease splits (it stays a crease) — splitting never
    // removes a feature, which is why it has by far the shortest refusal list.
    const DynamicEdge& edge_rec = surface.edges().at(edge);
    if (has_constraint(edge_rec.constraints, EdgeConstraint::UserLocked)) {
        out.result = TopologyResult::Constrained;
        return out;
    }

    const float param = std::clamp(t, 0.0f, 1.0f);
    const VertexId v0 = surface.origin_of(h0);
    const VertexId v1 = surface.origin_of(h1);
    if (!surface.live(v0) || !surface.live(v1)) return out;

    const bool side0 = surface.live(surface.face_of(h0));
    const bool side1 = surface.live(surface.face_of(h1));
    if (!side0 && !side1) return out;  // an edge with no face at all

    // -- decide ---------------------------------------------------------------
    //
    // Everything read here is read from the UNTOUCHED surface, and nothing is
    // written until every check has passed. That is the whole of atomicity.
    const DynamicVertex& a = surface.vertices().at(v0);
    const DynamicVertex& b = surface.vertices().at(v1);
    const kernel::cfloat3 pos = lerp3(a.position, b.position, param);

    // The opposite corners, one per live side.
    HalfEdgeId h0_next, h0_prev, h1_next, h1_prev;
    VertexId opp0, opp1;
    if (side0) {
        h0_next = surface.next_of(h0);
        h0_prev = surface.next_of(h0_next);
        if (!surface.live(h0_next) || !surface.live(h0_prev)) return out;
        opp0 = surface.origin_of(h0_prev);
    }
    if (side1) {
        h1_next = surface.next_of(h1);
        h1_prev = surface.next_of(h1_next);
        if (!surface.live(h1_next) || !surface.live(h1_prev)) return out;
        opp1 = surface.origin_of(h1_prev);
    }

    // The boundary loop's predecessor on each open side, captured BEFORE
    // anything is written: after the split the ghost has a different origin,
    // and the fan walk that finds this would follow the new state.
    const HalfEdgeId pred0 = side0 ? HalfEdgeId{} : boundary_predecessor(surface, h0);
    const HalfEdgeId pred1 = side1 ? HalfEdgeId{} : boundary_predecessor(surface, h1);

    // A split that would leave a triangle with no area is refused rather than
    // creating one for the next operator to trip over.
    if (side0 && (triangle_area_x2(a.position, pos, surface.position_of(opp0)) <
                      options.min_area_x2 ||
                  triangle_area_x2(pos, b.position, surface.position_of(opp0)) <
                      options.min_area_x2)) {
        out.result = TopologyResult::WouldDegenerate;
        return out;
    }
    if (side1 && (triangle_area_x2(b.position, pos, surface.position_of(opp1)) <
                      options.min_area_x2 ||
                  triangle_area_x2(pos, a.position, surface.position_of(opp1)) <
                      options.min_area_x2)) {
        out.result = TopologyResult::WouldDegenerate;
        return out;
    }

    // -- write ----------------------------------------------------------------
    DeltaScribe scribe{surface, delta, {}, {}, {}, {}};
    scribe.note(v0);
    scribe.note(v1);
    scribe.note(edge);
    scribe.note(h0);
    scribe.note(h1);
    if (side0) {
        scribe.note(h0_next);
        scribe.note(h0_prev);
        scribe.note(surface.face_of(h0));
        scribe.note(opp0);
    }
    if (side1) {
        scribe.note(h1_next);
        scribe.note(h1_prev);
        scribe.note(surface.face_of(h1));
        scribe.note(opp1);
    }

    DynamicVertex mid;
    mid.position = pos;
    mid.color = lerp3(a.color, b.color, param);
    mid.mask = lerp1(a.mask, b.mask, param);
    const VertexId vm = surface.create_vertex(mid);
    scribe.note_new(vm);

    // The corner UVs at each end of the edge, per side, so each side
    // interpolates its OWN pair. This is the whole of "a seam survives a
    // split": the two sides disagree about the UV and both are kept.
    const kernel::cfloat2 uv_h0_start = surface.halfedges().at(h0).uv;
    const kernel::cfloat2 uv_h1_start = surface.halfedges().at(h1).uv;
    const kernel::cfloat2 uv_mid_side0 = lerp2(uv_h0_start, uv_h1_start, param);
    const kernel::cfloat2 uv_mid_side1 = lerp2(uv_h1_start, uv_h0_start, 1.0f - param);

    // The new edge, from the midpoint to v1. `edge` keeps v0 -> midpoint, so a
    // handle to it still names the half of the edge nearest where it was.
    const EdgeId e_new = surface.create_edge(DynamicEdge{});
    surface.edges_mutable().at(e_new).constraints = edge_rec.constraints;
    scribe.note_new(e_new);

    // h0 keeps origin v0 and now targets vm; a new half-edge carries vm -> v1.
    DynamicHalfEdge proto;
    proto.origin = vm;
    proto.uv = uv_mid_side0;
    const HalfEdgeId h0b = surface.create_halfedge(proto);
    proto.origin = v1;
    proto.uv = surface.halfedges().at(h1).uv;
    const HalfEdgeId h1b = surface.create_halfedge(proto);
    scribe.note_new(h0b);
    scribe.note_new(h1b);
    // h1 keeps origin v1 -> ... so it must become vm's side. Re-origin it.
    surface.halfedges_mutable().at(h1).origin = vm;
    surface.halfedges_mutable().at(h1).uv = uv_mid_side1;

    surface.bind_edge(edge, h0, h1);    // v0 <-> vm
    surface.bind_edge(e_new, h0b, h1b);  // vm <-> v1

    out.vertex = vm;
    out.face_count = 0;

    if (side0) {
        // The old face becomes two: (v0, vm, opp0) and (vm, v1, opp0).
        const FaceId f_old = surface.face_of(h0);
        const FaceId f_new = surface.create_face(DynamicFace{});
        scribe.note_new(f_new);
        // The spoke from vm to opp0.
        const EdgeId e_spoke = surface.create_edge(DynamicEdge{});
        scribe.note_new(e_spoke);
        DynamicHalfEdge s0;
        s0.origin = vm;
        s0.uv = uv_mid_side0;
        DynamicHalfEdge s1;
        s1.origin = opp0;
        s1.uv = surface.halfedges().at(h0_prev).uv;
        const HalfEdgeId sp = surface.create_halfedge(s0);
        const HalfEdgeId sp_twin = surface.create_halfedge(s1);
        scribe.note_new(sp);
        scribe.note_new(sp_twin);
        surface.bind_edge(e_spoke, sp, sp_twin);

        // (v0 -> vm -> opp0): h0, sp, h0_prev
        surface.bind_face(f_old, h0, sp, h0_prev);
        // (vm -> v1 -> opp0): h0b, h0_next, sp_twin
        surface.bind_face(f_new, h0b, h0_next, sp_twin);
        out.faces[out.face_count++] = f_old;
        out.faces[out.face_count++] = f_new;
    } else {
        // A BOUNDARY HALF-EDGE has no face; what it has is a place in the border
        // loop, and the split has to put the new half-edge INTO that loop rather
        // than in front of it.
        //
        // h0 now runs v0 -> vm and h0b runs vm -> v1, so the border traverses
        // pred -> h0 -> h0b -> (whatever h0 pointed at). The first draft of this
        // wired it the other way round and produced a half-edge whose twin did
        // not start where it ended — which is exactly what the validator says.
        surface.halfedges_mutable().at(h0b).next = surface.halfedges().at(h0).next;
        surface.halfedges_mutable().at(h0).next = h0b;
        surface.halfedges_mutable().at(h0).face = FaceId{};
        surface.halfedges_mutable().at(h0b).face = FaceId{};
        if (surface.live(pred0)) surface.halfedges_mutable().at(pred0).next = h0;
    }

    if (side1) {
        const FaceId f_old = surface.face_of(h1);
        const FaceId f_new = surface.create_face(DynamicFace{});
        scribe.note_new(f_new);
        const EdgeId e_spoke = surface.create_edge(DynamicEdge{});
        scribe.note_new(e_spoke);
        DynamicHalfEdge s0;
        s0.origin = vm;
        s0.uv = uv_mid_side1;
        DynamicHalfEdge s1;
        s1.origin = opp1;
        s1.uv = surface.halfedges().at(h1_prev).uv;
        const HalfEdgeId sp = surface.create_halfedge(s0);
        const HalfEdgeId sp_twin = surface.create_halfedge(s1);
        scribe.note_new(sp);
        scribe.note_new(sp_twin);
        surface.bind_edge(e_spoke, sp, sp_twin);

        // h1 now runs vm -> v0, and h1b runs v1 -> vm.
        // (vm -> v0 -> opp1): h1, sp?  — careful: h1_prev ends at v1.
        // The old loop was h1(v1->v0), h1_next(v0->opp1), h1_prev(opp1->v1).
        // The two new triangles are:
        //   (v1 -> vm -> opp1): h1b, sp, h1_prev
        //   (vm -> v0 -> opp1): h1, h1_next, sp_twin
        surface.bind_face(f_new, h1b, sp, h1_prev);
        surface.bind_face(f_old, h1, h1_next, sp_twin);
        out.faces[out.face_count++] = f_old;
        out.faces[out.face_count++] = f_new;
    } else {
        // h1 now runs vm -> v0 and h1b runs v1 -> vm, so the border traverses
        // pred -> h1b -> h1 -> (whatever h1 pointed at). Note the ORDER is the
        // reverse of the side-0 case, because the split re-origined h1 rather
        // than leaving it where it started.
        surface.halfedges_mutable().at(h1b).next = h1;
        surface.halfedges_mutable().at(h1).face = FaceId{};
        surface.halfedges_mutable().at(h1b).face = FaceId{};
        if (surface.live(pred1)) surface.halfedges_mutable().at(pred1).next = h1b;
    }

    // THE OUTGOING SEEDS, set from what the operator knows rather than through
    // the ring walk. v1's seed was very likely h1, which the split re-origined
    // onto the midpoint; a walk starting there would traverse the wrong fan and
    // the validator would report a vertex pointing at a half-edge that does not
    // start where it says.
    std::vector<HalfEdgeId> seeds{h0, h1, h0b, h1b};
    if (side0) {
        seeds.push_back(h0_next);
        seeds.push_back(h0_prev);
    }
    if (side1) {
        seeds.push_back(h1_next);
        seeds.push_back(h1_prev);
    }
    surface.halfedges().for_each_live([&](HalfEdgeId id, const DynamicHalfEdge& he) {
        if (he.origin == vm) seeds.push_back(id);
    });
    reseat_outgoing(surface, v0, seeds);
    reseat_outgoing(surface, v1, seeds);
    reseat_outgoing(surface, vm, seeds);
    if (side0) reseat_outgoing(surface, opp0, seeds);
    if (side1) reseat_outgoing(surface, opp1, seeds);

    // Normals RECOMPUTED, not interpolated: an interpolated normal on a curved
    // surface is not the surface's normal, and every later operator would
    // inherit the error.
    std::vector<FaceId> touched(out.faces, out.faces + out.face_count);
    surface.refresh_normals(touched);

    // Sync everything the write phase touched, over the list the scribe kept —
    // which is the neighbourhood, not the surface.
    scribe.sync_all();

    surface.bump_topology();
    surface.bump_geometry();
    out.result = TopologyResult::Ok;
    return out;
}

// -- collapse -----------------------------------------------------------------

CollapseResult collapse_edge(DynamicSurface& surface, EdgeId edge,
                             const TopologyOpOptions& options, TopologyDelta* delta) {
    CollapseResult out;
    if (!surface.live(edge)) return out;

    const HalfEdgeId h0 = surface.halfedge_of(edge);
    const HalfEdgeId h1 = surface.twin_of(h0);
    if (!surface.live(h0) || !surface.live(h1)) return out;

    const DynamicEdge& edge_rec = surface.edges().at(edge);
    if ((edge_rec.constraints & options.collapse_blockers) != 0) {
        out.result = TopologyResult::Constrained;
        return out;
    }

    const VertexId v0 = surface.origin_of(h0);
    const VertexId v1 = surface.origin_of(h1);
    if (!surface.live(v0) || !surface.live(v1)) return out;

    // -- the link condition ---------------------------------------------------
    //
    // THE TOPOLOGICAL test, and the reason a geometric one is not enough: a
    // collapse whose endpoints share a neighbour that is NOT opposite the edge
    // pinches the surface into a non-manifold state. It looks fine in a render
    // and is unusable afterwards — the two faces that meet at the pinch have no
    // consistent orientation between them, and every later walk through that
    // vertex visits both sides of a surface that no longer has two sides.
    std::vector<VertexId> ring0, ring1;
    if (!surface.one_ring(v0, &ring0) || !surface.one_ring(v1, &ring1)) return out;

    // The vertices opposite the edge: the third corner of each incident face.
    std::vector<VertexId> opposite;
    for (HalfEdgeId h : {h0, h1}) {
        if (!surface.live(surface.face_of(h))) continue;
        const HalfEdgeId prev = surface.next_of(surface.next_of(h));
        if (!surface.live(prev)) return out;
        opposite.push_back(surface.origin_of(prev));
    }

    std::vector<std::uint32_t> shared;
    {
        std::vector<std::uint32_t> a, b;
        for (VertexId v : ring0) a.push_back(v.slot);
        for (VertexId v : ring1) b.push_back(v.slot);
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(shared));
    }
    if (shared.size() != opposite.size()) {
        out.result = TopologyResult::LinkCondition;
        return out;
    }
    for (std::uint32_t s : shared) {
        bool is_opposite = false;
        for (VertexId o : opposite)
            if (o.slot == s) is_opposite = true;
        if (!is_opposite) {
            out.result = TopologyResult::LinkCondition;
            return out;
        }
    }

    if (ring0.size() < 3 || ring1.size() < 3) {
        out.result = TopologyResult::LinkCondition;
        return out;
    }

    // WOULD ANY TWO FACES BECOME THE SAME TRIANGLE?
    //
    // The link condition is necessary and not sufficient, and the tetrahedron is
    // the case that shows it: every one of its edges passes the link test — the
    // endpoints' rings intersect in exactly the two opposite vertices — and
    // collapsing any of them leaves two faces on the same three vertices, which
    // is a surface with no interior.
    //
    // So the surviving faces are enumerated with the merge applied and checked
    // for repeats. This is the "duplicate triangle" refusal the requirement
    // names, and it is not reachable from the link condition alone.
    {
        std::vector<FaceId> around;
        std::vector<std::array<std::uint32_t, 3>> triples;
        for (VertexId v : {v0, v1}) {
            if (!surface.incident_faces(v, &around)) return out;
            for (FaceId f : around) {
                bool dies = false;
                for (HalfEdgeId h : {h0, h1})
                    if (surface.face_of(h) == f) dies = true;
                if (dies) continue;
                VertexId tri[3];
                if (!surface.face_vertices(f, tri)) return out;
                std::array<std::uint32_t, 3> key{};
                for (int i = 0; i < 3; ++i)
                    key[i] = (tri[i] == v1) ? v0.slot : tri[i].slot;
                std::sort(key.begin(), key.end());
                triples.push_back(key);
            }
        }
        std::sort(triples.begin(), triples.end());
        if (std::adjacent_find(triples.begin(), triples.end()) != triples.end()) {
            out.result = TopologyResult::LinkCondition;
            return out;
        }
    }

    // -- placement ------------------------------------------------------------
    //
    // Midpoint, except that a constraint wins over geometry: exactly one
    // constrained endpoint keeps its position so the feature does not move, and
    // both constrained is refused unless the edge itself carries the same
    // constraint, in which case the feature collapses along itself. See D12.
    const bool b0 = surface.is_boundary_vertex(v0);
    const bool b1 = surface.is_boundary_vertex(v1);
    const bool edge_is_boundary = has_constraint(edge_rec.constraints, EdgeConstraint::Boundary);
    kernel::cfloat3 target;
    if (b0 && b1 && !edge_is_boundary) {
        // Two border vertices joined by an INTERIOR edge: collapsing them
        // welds two parts of the border together, which changes the surface's
        // shape in a way no local rule can justify.
        out.result = TopologyResult::LinkCondition;
        return out;
    } else if (b0 && !b1) {
        target = surface.position_of(v0);
    } else if (b1 && !b0) {
        target = surface.position_of(v1);
    } else {
        target = surface.edge_midpoint(edge);
    }

    // -- geometric refusals ---------------------------------------------------
    //
    // What the link condition cannot see: a collapse that is topologically fine
    // and folds the surface over itself.
    std::vector<FaceId> faces0, faces1;
    if (!surface.incident_faces(v0, &faces0) || !surface.incident_faces(v1, &faces1)) return out;

    std::vector<FaceId> dying;
    for (HalfEdgeId h : {h0, h1}) {
        const FaceId f = surface.face_of(h);
        if (surface.live(f)) dying.push_back(f);
    }
    auto is_dying = [&](FaceId f) {
        for (FaceId d : dying)
            if (d == f) return true;
        return false;
    };

    for (const std::vector<FaceId>* list : {&faces0, &faces1}) {
        for (FaceId f : *list) {
            if (is_dying(f)) continue;
            VertexId v[3];
            if (!surface.face_vertices(f, v)) return out;
            kernel::cfloat3 p[3];
            for (int i = 0; i < 3; ++i)
                p[i] = (v[i] == v0 || v[i] == v1) ? target : surface.position_of(v[i]);
            if (triangle_area_x2(p[0], p[1], p[2]) < options.min_area_x2) {
                out.result = TopologyResult::WouldDegenerate;
                return out;
            }
            const kernel::cfloat3 before = surface.face_normal(f);
            const kernel::cfloat3 after = triangle_normal(p[0], p[1], p[2]);
            const float dot = std::clamp(kernel::cdot(before, after), -1.0f, 1.0f);
            if (std::acos(dot) > options.max_normal_swing) {
                out.result = TopologyResult::NormalFlip;
                return out;
            }
        }
    }

    // THE BORDER LOOP, captured before anything is written.
    //
    // Collapsing a boundary edge erases its ghost half-edge, and the ghost
    // BEFORE it in the border loop is then pointing at a dead one. Nothing in
    // the fan rewiring notices, because the predecessor is not in either
    // endpoint's fan — it is one step further round the border. The fuzz run
    // found this at seed 2 step 40, as "half-edge 156 has a dead next", which
    // is exactly the shape of defect that only a randomized interleaving
    // reaches.
    HalfEdgeId border_pred, border_succ;
    for (HalfEdgeId ghost : {h0, h1}) {
        if (!surface.is_boundary_halfedge(ghost)) continue;
        border_pred = boundary_predecessor(surface, ghost);
        border_succ = surface.next_of(ghost);
        break;
    }

    // -- write ----------------------------------------------------------------
    DeltaScribe scribe{surface, delta, {}, {}, {}, {}};
    if (surface.live(border_pred)) scribe.note(border_pred);
    // Everything within two rings of the edge, because a collapse rewires the
    // half-edges of both fans and the faces that survive on either side.
    scribe.note(v0);
    scribe.note(v1);
    scribe.note(edge);
    for (VertexId v : ring0) scribe.note(v);
    for (VertexId v : ring1) scribe.note(v);
    for (FaceId f : faces0) scribe.note(f);
    for (FaceId f : faces1) scribe.note(f);
    std::vector<HalfEdgeId> ring_h;
    for (VertexId v : {v0, v1}) {
        if (surface.outgoing_halfedges(v, &ring_h))
            for (HalfEdgeId h : ring_h) {
                scribe.note(h);
                scribe.note(surface.twin_of(h));
                scribe.note(surface.next_of(h));
                scribe.note(surface.edge_of(h));
                scribe.note(surface.face_of(h));
                // The far side of the fan's faces too.
                const HalfEdgeId n = surface.next_of(h);
                scribe.note(surface.twin_of(n));
                const HalfEdgeId nn = surface.next_of(n);
                scribe.note(nn);
                scribe.note(surface.twin_of(nn));
                scribe.note(surface.edge_of(nn));
            }
    }

    // v0 survives; v1's half-edges are re-originated onto it.
    surface.vertices_mutable().at(v0).position = target;
    surface.vertices_mutable().at(v0).color =
        (surface.vertices().at(v0).color + surface.vertices().at(v1).color) * 0.5f;
    surface.vertices_mutable().at(v0).mask =
        (surface.vertices().at(v0).mask + surface.vertices().at(v1).mask) * 0.5f;

    if (surface.outgoing_halfedges(v1, &ring_h))
        for (HalfEdgeId h : ring_h)
            if (surface.live(h)) surface.halfedges_mutable().at(h).origin = v0;

    // Each dying face welds its two remaining edges into one: the half-edges
    // opposite the collapsed edge become twins of each other.
    for (HalfEdgeId h : {h0, h1}) {
        const FaceId f = surface.face_of(h);
        if (!surface.live(f)) continue;
        const HalfEdgeId n = surface.next_of(h);
        const HalfEdgeId p = surface.next_of(n);
        if (!surface.live(n) || !surface.live(p)) continue;
        const HalfEdgeId tn = surface.twin_of(n);
        const HalfEdgeId tp = surface.twin_of(p);
        const EdgeId keep = surface.edge_of(p);
        const EdgeId drop = surface.edge_of(n);
        // Keep `p`'s edge and merge `n`'s constraints into it, so a seam or a
        // crease running through the collapsed triangle survives on the edge
        // that replaces the pair.
        if (surface.live(keep) && surface.live(drop))
            surface.edges_mutable().at(keep).constraints |=
                surface.edges().at(drop).constraints;
        if (surface.live(tn) && surface.live(tp)) surface.bind_edge(keep, tn, tp);
        // The opposite corner may have pointed at a half-edge that is about to
        // die.
        surface.erase_halfedge(n);
        surface.erase_halfedge(p);
        surface.erase_edge(drop);
        surface.erase_face(f);
    }

    surface.erase_halfedge(h0);
    surface.erase_halfedge(h1);
    surface.erase_edge(edge);
    surface.erase_vertex(v1);

    // Close the border loop over the ghost that just went away.
    if (surface.live(border_pred) && surface.live(border_succ))
        surface.halfedges_mutable().at(border_pred).next = border_succ;

    // THE BOUNDARY FLAGS have to agree with the incidence afterwards, or a
    // remesher trusting them protects the wrong edges and the validator says
    // so. A collapse can turn an interior edge into a boundary one by welding
    // a ghost onto it, which is not obvious from the rewiring.
    {
        std::vector<HalfEdgeId> fan;
        if (surface.outgoing_halfedges(v0, &fan))
            for (HalfEdgeId h : fan) {
                const EdgeId e = surface.edge_of(h);
                if (!surface.live(e)) continue;
                std::uint32_t& flags = surface.edges_mutable().at(e).constraints;
                if (surface.is_boundary_edge(e))
                    flags |= EdgeConstraint::Boundary;
                else
                    flags &= ~static_cast<std::uint32_t>(EdgeConstraint::Boundary);
            }
    }

    // EVERY VERTEX WHOSE FAN WAS REWIRED needs a live outgoing again, and a ring
    // walk cannot supply one: a collapse erases six half-edges, and a vertex
    // whose seed was one of them has no valid place for the walk to start.
    // The surviving half-edges of the neighbourhood are where to look.
    std::vector<HalfEdgeId> survivors;
    surface.halfedges().for_each_live(
        [&](HalfEdgeId id, const DynamicHalfEdge&) { survivors.push_back(id); });
    reseat_outgoing(surface, v0, survivors);
    for (VertexId v : ring0)
        if (surface.live(v)) reseat_outgoing(surface, v, survivors);
    for (VertexId v : ring1)
        if (surface.live(v)) reseat_outgoing(surface, v, survivors);

    out.faces.clear();
    if (surface.incident_faces(v0, &out.faces)) surface.refresh_normals(out.faces);

    scribe.sync_all();

    surface.bump_topology();
    surface.bump_geometry();
    out.kept = v0;
    out.removed = v1;
    out.result = TopologyResult::Ok;
    return out;
}

// -- flip ---------------------------------------------------------------------

FlipResult flip_edge(DynamicSurface& surface, EdgeId edge, const TopologyOpOptions& options,
                     TopologyDelta* delta, bool force) {
    FlipResult out;
    if (!surface.live(edge)) return out;

    const HalfEdgeId h0 = surface.halfedge_of(edge);
    const HalfEdgeId h1 = surface.twin_of(h0);
    if (!surface.live(h0) || !surface.live(h1)) return out;

    const FaceId f0 = surface.face_of(h0);
    const FaceId f1 = surface.face_of(h1);
    // A boundary edge has one face and no second diagonal to flip to.
    if (!surface.live(f0) || !surface.live(f1)) {
        out.result = TopologyResult::Constrained;
        return out;
    }
    const DynamicEdge& edge_rec = surface.edges().at(edge);
    if ((edge_rec.constraints & options.flip_blockers) != 0) {
        out.result = TopologyResult::Constrained;
        return out;
    }

    const HalfEdgeId a1 = surface.next_of(h0);
    const HalfEdgeId a2 = surface.next_of(a1);
    const HalfEdgeId b1 = surface.next_of(h1);
    const HalfEdgeId b2 = surface.next_of(b1);
    if (!surface.live(a1) || !surface.live(a2) || !surface.live(b1) || !surface.live(b2))
        return out;

    const VertexId v0 = surface.origin_of(h0);
    const VertexId v1 = surface.origin_of(h1);
    const VertexId oa = surface.origin_of(a2);  // opposite on f0
    const VertexId ob = surface.origin_of(b2);  // opposite on f1
    if (!surface.live(oa) || !surface.live(ob)) return out;

    // THE NEW DIAGONAL MUST NOT ALREADY EXIST. Two edges between one pair of
    // vertices is not a surface; it renders and every walk through either one
    // afterwards is ambiguous.
    std::vector<VertexId> ring;
    if (!surface.one_ring(oa, &ring)) return out;
    for (VertexId v : ring)
        if (v == ob) {
            out.result = TopologyResult::DiagonalExists;
            return out;
        }

    // A vertex of valence 3 losing an edge is left with two, which is a
    // degenerate fan.
    if (surface.valence(v0) <= 3 || surface.valence(v1) <= 3) {
        out.result = TopologyResult::LinkCondition;
        return out;
    }

    const kernel::cfloat3 p_v0 = surface.position_of(v0);
    const kernel::cfloat3 p_v1 = surface.position_of(v1);
    const kernel::cfloat3 p_oa = surface.position_of(oa);
    const kernel::cfloat3 p_ob = surface.position_of(ob);

    // Neither new face may be degenerate or inverted relative to the pair it
    // replaces.
    if (triangle_area_x2(p_oa, p_ob, p_v1) < options.min_area_x2 ||
        triangle_area_x2(p_ob, p_oa, p_v0) < options.min_area_x2) {
        out.result = TopologyResult::WouldDegenerate;
        return out;
    }
    const kernel::cfloat3 before = surface.face_normal(f0);
    const kernel::cfloat3 n_a = triangle_normal(p_oa, p_ob, p_v1);
    const kernel::cfloat3 n_b = triangle_normal(p_ob, p_oa, p_v0);
    if (std::acos(std::clamp(kernel::cdot(before, n_a), -1.0f, 1.0f)) > options.max_normal_swing ||
        std::acos(std::clamp(kernel::cdot(before, n_b), -1.0f, 1.0f)) > options.max_normal_swing) {
        out.result = TopologyResult::WouldInvert;
        return out;
    }

    // A flip is only worth doing if it IMPROVES the pair. The remesher asks
    // about far more edges than it flips, so this is the common answer rather
    // than an error.
    if (!force) {
        const float now = edge_pair_quality(surface, edge);
        const float then = std::min(min_angle(p_oa, p_ob, p_v1), min_angle(p_ob, p_oa, p_v0));
        if (!(then > now)) {
            out.result = TopologyResult::NoImprovement;
            return out;
        }
    }

    // -- write ----------------------------------------------------------------
    DeltaScribe scribe{surface, delta, {}, {}, {}, {}};
    for (HalfEdgeId h : {h0, h1, a1, a2, b1, b2}) {
        scribe.note(h);
        scribe.note(surface.twin_of(h));
    }
    scribe.note(v0);
    scribe.note(v1);
    scribe.note(oa);
    scribe.note(ob);
    scribe.note(f0);
    scribe.note(f1);
    scribe.note(edge);

    // The edge becomes oa -> ob. Its two half-edges are re-originated, and the
    // two faces are rebound to the other diagonal.
    surface.halfedges_mutable().at(h0).origin = oa;
    surface.halfedges_mutable().at(h1).origin = ob;
    surface.halfedges_mutable().at(h0).uv = surface.halfedges().at(a2).uv;
    surface.halfedges_mutable().at(h1).uv = surface.halfedges().at(b2).uv;

    // f0 becomes (oa, ob, v1): h0, b2, a1
    surface.bind_face(f0, h0, b2, a1);
    // f1 becomes (ob, oa, v0): h1, a2, b1
    surface.bind_face(f1, h1, a2, b1);

    // The vertices that lost their outgoing to the rewire. h0 and h1 changed
    // origin, so a seed pointing at either is now wrong.
    const std::vector<HalfEdgeId> seeds{h0, h1, a1, a2, b1, b2};
    reseat_outgoing(surface, v0, seeds);
    reseat_outgoing(surface, v1, seeds);
    reseat_outgoing(surface, oa, seeds);
    reseat_outgoing(surface, ob, seeds);

    out.faces[0] = f0;
    out.faces[1] = f1;
    std::vector<FaceId> touched{f0, f1};
    surface.refresh_normals(touched);

    scribe.sync_all();

    surface.bump_topology();
    surface.bump_geometry();
    out.result = TopologyResult::Ok;
    return out;
}

}  // namespace mesh
}  // namespace clay
