#include "clay/mesh/dynamic_validate.h"

#include <cmath>
#include <string>
#include <vector>

namespace clay {
namespace mesh {
namespace {

bool finite3(kernel::cfloat3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::string s(std::uint32_t v) { return std::to_string(v); }

// THE POOL UNDER THE SURFACE, which every other check here is blind to.
//
// The checks below this one walk the connectivity, and connectivity can be
// FLAWLESS while the pool it lives in is wrecked: a free list that names live
// slots, names them twice, and cycles. That is not a theoretical gap. An undo
// followed by a redo left exactly that state -- every slot, generation, link,
// position and attribute identical to before the history operation, this
// validator passing, and the free list holding 201 live slots against 16 dead
// with a cycle in it. `SlotPool::create` walks that list, so the surface was
// not wrong anywhere; the next dab simply never returned.
//
// A validator whose header promises "every invariant a half-edge surface has"
// has to include the storage, or the next defect of this shape hides in exactly
// the same place.
template <typename Pool>
void check_pool(const Pool& pool, const char* kind, DynamicValidationReport* report) {
    std::size_t visited = 0, live_on_list = 0;
    bool cycle = false;
    if (pool.free_list_intact(&visited, &cycle, &live_on_list)) return;
    if (cycle) {
        report->add(std::string(kind) + " free list cycles", 0);
        return;  // the counts below are meaningless once the walk was cut short
    }
    if (live_on_list)
        report->add(std::string(kind) + " free list holds " + s(static_cast<std::uint32_t>(live_on_list)) +
                        " live slot(s)",
                    0);
    if (visited != pool.dead_slots())
        report->add(std::string(kind) + " free list names " +
                        s(static_cast<std::uint32_t>(visited)) + " slot(s) but " +
                        s(static_cast<std::uint32_t>(pool.dead_slots())) + " are dead",
                    0);
}

// The face-loop check, shared by the full and the local validator: three steps,
// closing, every corner naming this face and a live origin, and no repeats.
void check_face(const DynamicSurface& surface, FaceId f, DynamicValidationReport* report) {
    const DynamicFace* rec = surface.face(f);
    if (!rec) return;
    if (!surface.live(rec->halfedge)) {
        report->add("face " + s(f.slot) + " names a dead half-edge", f.slot);
        return;
    }
    HalfEdgeId h = rec->halfedge;
    VertexId seen[3];
    for (int i = 0; i < 3; ++i) {
        const DynamicHalfEdge* he = surface.halfedge(h);
        if (!he) {
            report->add("face " + s(f.slot) + " loop hits a dead half-edge", f.slot);
            return;
        }
        if (!(he->face == f)) {
            report->add("half-edge " + s(h.slot) + " is in face " + s(f.slot) +
                            "'s loop but names another face",
                        h.slot);
            return;
        }
        if (!surface.live(he->origin)) {
            report->add("half-edge " + s(h.slot) + " originates at a dead vertex", h.slot);
            return;
        }
        seen[i] = he->origin;
        h = he->next;
    }
    if (!(h == rec->halfedge)) {
        report->add("face " + s(f.slot) + " does not close in three half-edges", f.slot);
        return;
    }
    // A face with a repeated corner has no area and no normal, and every
    // operator downstream would have to guard against it.
    if (seen[0] == seen[1] || seen[1] == seen[2] || seen[0] == seen[2])
        report->add("face " + s(f.slot) + " repeats a vertex", f.slot);
}

void check_halfedge(const DynamicSurface& surface, HalfEdgeId h, DynamicValidationReport* report) {
    const DynamicHalfEdge* he = surface.halfedge(h);
    if (!he) return;
    if (!surface.live(he->twin)) {
        report->add("half-edge " + s(h.slot) + " has a dead twin", h.slot);
        return;
    }
    if (he->twin == h) {
        report->add("half-edge " + s(h.slot) + " is its own twin", h.slot);
        return;
    }
    const DynamicHalfEdge* tw = surface.halfedge(he->twin);
    if (!(tw->twin == h)) {
        report->add("twin of twin of half-edge " + s(h.slot) + " is not itself", h.slot);
        return;
    }
    if (!(tw->edge == he->edge))
        report->add("half-edge " + s(h.slot) + " and its twin name different edges", h.slot);
    if (!surface.live(he->edge))
        report->add("half-edge " + s(h.slot) + " names a dead edge", h.slot);
    if (!surface.live(he->origin))
        report->add("half-edge " + s(h.slot) + " originates at a dead vertex", h.slot);
    if (!surface.live(he->next))
        report->add("half-edge " + s(h.slot) + " has a dead next", h.slot);
    // The pair runs between the same two vertices, opposite ways. A twin whose
    // origin is not this half-edge's target is the single most common way a
    // hand-written operator goes wrong.
    const VertexId target = surface.origin_of(he->next);
    if (target.valid() && !(tw->origin == target))
        report->add("half-edge " + s(h.slot) + "'s twin does not start where it ends", h.slot);
}

}  // namespace

DynamicValidationReport validate_dynamic_surface(const DynamicSurface& surface) {
    DynamicValidationReport report;

    // The storage first. A wrecked free list does not break any check below, so
    // reporting it after them would bury the one issue that explains a hang.
    check_pool(surface.vertices(), "vertex", &report);
    check_pool(surface.halfedges(), "half-edge", &report);
    check_pool(surface.edges(), "edge", &report);
    check_pool(surface.faces(), "face", &report);

    surface.halfedges().for_each_live(
        [&](HalfEdgeId id, const DynamicHalfEdge&) { check_halfedge(surface, id, &report); });

    surface.faces().for_each_live(
        [&](FaceId id, const DynamicFace& f) {
            check_face(surface, id, &report);
            if (!finite3(f.normal)) report.add("face " + s(id.slot) + " has a non-finite normal", id.slot);
        });

    surface.edges().for_each_live([&](EdgeId id, const DynamicEdge& e) {
        if (!surface.live(e.halfedge)) {
            report.add("edge " + s(id.slot) + " names a dead half-edge", id.slot);
            return;
        }
        const HalfEdgeId a = e.halfedge;
        const HalfEdgeId b = surface.twin_of(a);
        if (!surface.live(b)) {
            report.add("edge " + s(id.slot) + " has only one half-edge", id.slot);
            return;
        }
        if (!(surface.edge_of(a) == id) || !(surface.edge_of(b) == id))
            report.add("edge " + s(id.slot) + " and its half-edges disagree", id.slot);
        // The flag has to agree with the incidence, or a remesher trusting the
        // flag protects the wrong edges.
        const bool actually_boundary =
            surface.is_boundary_halfedge(a) || surface.is_boundary_halfedge(b);
        const bool flagged = has_constraint(e.constraints, EdgeConstraint::Boundary);
        if (actually_boundary != flagged)
            report.add("edge " + s(id.slot) + " boundary flag disagrees with its incidence",
                       id.slot);
    });

    std::vector<HalfEdgeId> ring;
    surface.vertices().for_each_live([&](VertexId id, const DynamicVertex& v) {
        if (!finite3(v.position))
            report.add("vertex " + s(id.slot) + " has a non-finite position", id.slot);
        if (!finite3(v.normal))
            report.add("vertex " + s(id.slot) + " has a non-finite normal", id.slot);
        if (!surface.live(v.outgoing)) {
            report.add("vertex " + s(id.slot) + " has a dead outgoing half-edge", id.slot);
            return;
        }
        if (!(surface.origin_of(v.outgoing) == id))
            report.add("vertex " + s(id.slot) + "'s outgoing half-edge starts elsewhere", id.slot);
        // The ring must close, or terminate on a boundary. A walk that runs out
        // of its bound is a corrupted fan, which is exactly the failure that
        // renders fine and breaks later.
        if (!surface.outgoing_halfedges(id, &ring))
            report.add("vertex " + s(id.slot) + "'s ring does not close or open cleanly", id.slot);
    });

    return report;
}

DynamicValidationReport validate_local(const DynamicSurface& surface,
                                const std::vector<FaceId>& faces) {
    DynamicValidationReport report;
    for (FaceId f : faces) {
        if (!surface.live(f)) continue;
        check_face(surface, f, &report);
        const DynamicFace* rec = surface.face(f);
        if (!rec) continue;
        HalfEdgeId h = rec->halfedge;
        for (int i = 0; i < 3 && surface.live(h); ++i) {
            check_halfedge(surface, h, &report);
            // The twin's face too: a collapse's damage usually shows on the
            // other side of an edge it rewired.
            const HalfEdgeId tw = surface.twin_of(h);
            if (surface.live(tw)) check_halfedge(surface, tw, &report);
            h = surface.next_of(h);
        }
    }
    return report;
}

}  // namespace mesh
}  // namespace clay
