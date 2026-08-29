#include "clay/mesh/remesh_local.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace clay {
namespace mesh {

RemeshTiming default_timing(MeshBrush verb) {
    switch (verb) {
        // GRAB and SNAKEHOOK stretch what they touch, and the stretch is the
        // thing that needs refining. Remeshing first would refine triangles the
        // drag is about to pull apart anyway; remeshing after is what turns a
        // pulled tendril into a tendril with geometry in it. BOTH for snakehook,
        // because it re-anchors between stamps and needs somewhere to stand.
        case MeshBrush::Grab:
            return RemeshTiming::AfterBrush;
        case MeshBrush::Snakehook:
            return RemeshTiming::BeforeAndAfter;

        // The DEPOSIT family needs the geometry before it deposits: material
        // added onto triangles too coarse to hold its shape is a smooth bump
        // where the brush promised an edge.
        case MeshBrush::Draw:
        case MeshBrush::Clay:
        case MeshBrush::Crease:
        case MeshBrush::Inflate:
        case MeshBrush::Layer:
            return RemeshTiming::BeforeBrush;

        // SMOOTH and its family REMOVE detail, so remeshing first gives the
        // pass something to work with and remeshing after would put back what
        // it just took out. Collapse is what actually thins the region, and it
        // runs in the same pass.
        case MeshBrush::Smooth:
        case MeshBrush::Polish:
        case MeshBrush::Scrape:
            return RemeshTiming::BeforeBrush;

        // RELAX is remeshing's own sibling: it evens spacing without changing
        // counts. Running the adaptive pass after it means the relax has
        // already put the vertices where the length test will read them.
        case MeshBrush::Relax:
            return RemeshTiming::AfterBrush;

        case MeshBrush::Flatten:
        case MeshBrush::Pinch:
        case MeshBrush::Nudge:
            return RemeshTiming::BeforeBrush;

        // The colour pair changes no geometry at all, so there is nothing to
        // adapt to. Named rather than defaulted, so adding a verb still has to
        // answer this question.
        case MeshBrush::Paint:
        case MeshBrush::Smear:
            return RemeshTiming::BeforeBrush;
    }
    return RemeshTiming::BeforeBrush;
}

namespace {

// Which constraints stop this settings block from touching an edge.
std::uint32_t blockers_for(const DynamicTopologySettings& s) {
    std::uint32_t mask = static_cast<std::uint32_t>(EdgeConstraint::UserLocked);
    if (s.preserve_boundaries) mask |= EdgeConstraint::Boundary;
    if (s.preserve_uv_seams) mask |= EdgeConstraint::UvSeam;
    if (s.preserve_sharp_edges) mask |= EdgeConstraint::Sharp;
    return mask;
}

// The edges of every face the ball reaches, deduplicated and IN SLOT ORDER.
//
// The order is the determinism rule: a query returns faces in the tree's
// traversal order, the tree's shape depends on the history of edits, and a
// remesher that split them in that order would produce a different surface on a
// differently-edited-but-identical input. Sorting by slot removes the
// dependency entirely.
void gather_edges(const DynamicSurface& surface, DynamicBvh* bvh, kernel::cfloat3 centre,
                  float radius, std::vector<EdgeId>* out) {
    out->clear();
    std::vector<FaceId> faces;
    if (bvh) {
        bvh->faces_in_ball(surface, centre, radius, &faces);
    } else {
        const float r2 = radius * radius;
        surface.faces().for_each_live([&](FaceId f, const DynamicFace&) {
            VertexId v[3];
            if (!surface.face_vertices(f, v)) return;
            for (int i = 0; i < 3; ++i)
                if (kernel::cdot2(surface.position_of(v[i]) - centre) <= r2) {
                    faces.push_back(f);
                    return;
                }
        });
    }
    for (FaceId f : faces) {
        const DynamicFace* rec = surface.face(f);
        if (!rec) continue;
        HalfEdgeId h = rec->halfedge;
        for (int i = 0; i < 3 && surface.live(h); ++i) {
            const EdgeId e = surface.edge_of(h);
            if (surface.live(e)) out->push_back(e);
            h = surface.next_of(h);
        }
    }
    std::sort(out->begin(), out->end(), [](EdgeId a, EdgeId b) { return a.slot < b.slot; });
    out->erase(std::unique(out->begin(), out->end(),
                           [](EdgeId a, EdgeId b) { return a.slot == b.slot; }),
               out->end());
}

void feed_index(DynamicSurface& surface, DynamicBvh* bvh, const FaceId* faces, int count) {
    if (!bvh) return;
    for (int i = 0; i < count; ++i) {
        if (!faces[i].valid()) continue;
        bvh->erase(faces[i]);
        if (surface.live(faces[i])) bvh->insert(surface, faces[i]);
    }
}

void count_refusal(TopologyResult r, RemeshStats* stats) {
    switch (r) {
        case TopologyResult::Constrained:
            ++stats->refused_constrained;
            break;
        case TopologyResult::LinkCondition:
        case TopologyResult::DiagonalExists:
            ++stats->refused_topology;
            break;
        case TopologyResult::WouldInvert:
        case TopologyResult::WouldDegenerate:
        case TopologyResult::NormalFlip:
            ++stats->refused_geometry;
            break;
        default:
            break;
    }
}

}  // namespace

RemeshStats remesh_region(DynamicSurface& surface, DynamicBvh* bvh, kernel::cfloat3 centre,
                          float radius, const DynamicTopologySettings& settings,
                          TopologyDelta* delta) {
    RemeshStats stats;
    if (!settings.enabled || radius <= 0.0f) return stats;
    const float target = settings.target_for(radius);
    if (!(target > 0.0f)) return stats;  // Constant mode: no adaptation

    TopologyOpOptions op = settings.op;
    op.collapse_blockers = blockers_for(settings);
    // A flip is stopped by every constraint including the boundary, because a
    // flip MOVES the edge and a constrained edge that moved is the constraint
    // deleted.
    op.flip_blockers = blockers_for(settings) | EdgeConstraint::Boundary;

    const float split_above = target * settings.split_factor;
    const float collapse_below = target * settings.collapse_factor;
    // The region is grown a little for the QUERY, so an edge whose far end lies
    // just outside the brush is still considered — otherwise the rim of every
    // stamp is a ring of edges nobody ever adapts.
    const float query_radius = radius * 1.25f;

    int budget = settings.max_ops_per_stamp;
    std::vector<EdgeId> edges;

    for (int pass = 0; pass < settings.max_passes && budget > 0; ++pass) {
        const std::size_t before = stats.total();

        // -- SPLIT ------------------------------------------------------------
        if (settings.allow_split) {
            gather_edges(surface, bvh, centre, query_radius, &edges);
            for (EdgeId e : edges) {
                if (budget <= 0) {
                    stats.hit_budget = true;
                    break;
                }
                if (!surface.live(e)) continue;
                if (surface.edge_length(e) <= split_above) continue;
                const SplitResult r = split_edge(surface, e, 0.5f, op, delta);
                if (r.result == TopologyResult::Ok) {
                    ++stats.split;
                    --budget;
                    feed_index(surface, bvh, r.faces, r.face_count);
                } else {
                    count_refusal(r.result, &stats);
                }
            }
        }

        // -- COLLAPSE ---------------------------------------------------------
        //
        // The hysteresis lives between the two thresholds above: an edge split
        // in this pass is `target * split_factor / 2` long, which is well above
        // `target * collapse_factor`, so it cannot be collapsed back by the
        // very next pass.
        if (settings.allow_collapse) {
            gather_edges(surface, bvh, centre, query_radius, &edges);
            for (EdgeId e : edges) {
                if (budget <= 0) {
                    stats.hit_budget = true;
                    break;
                }
                if (!surface.live(e)) continue;
                if (surface.edge_length(e) >= collapse_below) continue;

                // WOULD THE COLLAPSE CREATE AN EDGE THE SPLIT PASS WILL SPLIT?
                //
                // This is the guard that makes the hysteresis actually work, and
                // leaving it out is why the first version of this file
                // oscillated: with split at 1.33x and collapse at 0.8x, an edge
                // just over the split threshold becomes two of 0.67x, which are
                // BELOW the collapse threshold, so they collapse straight back
                // and the pair pumps for as long as the brush is held still.
                // The gap between the two factors cannot fix that on its own —
                // no gap can, because splitting halves a length and the two
                // thresholds are less than a factor of two apart.
                //
                // What fixes it is refusing a collapse whose result would be too
                // long, which is the standard incremental-remeshing rule and the
                // reason the classic factors work at all.
                {
                    const HalfEdgeId ch = surface.halfedge_of(e);
                    const VertexId keep = surface.origin_of(ch);
                    const VertexId gone = surface.target_of(ch);
                    const kernel::cfloat3 target_pos = surface.edge_midpoint(e);
                    bool too_long = false;
                    std::vector<VertexId> ring;
                    for (VertexId v : {keep, gone}) {
                        if (too_long || !surface.one_ring(v, &ring)) continue;
                        for (VertexId n : ring) {
                            if (n == keep || n == gone) continue;
                            if (kernel::clength(surface.position_of(n) - target_pos) >
                                split_above) {
                                too_long = true;
                                break;
                            }
                        }
                    }
                    if (too_long) continue;
                }

                // The faces about to disappear have to leave the index before
                // they leave the surface, or it holds handles to dead faces.
                const HalfEdgeId h = surface.halfedge_of(e);
                const FaceId dying[2] = {surface.face_of(h),
                                         surface.face_of(surface.twin_of(h))};
                const CollapseResult r = collapse_edge(surface, e, op, delta);
                if (r.result == TopologyResult::Ok) {
                    ++stats.collapsed;
                    --budget;
                    if (bvh) {
                        for (FaceId f : dying) bvh->erase(f);
                        for (FaceId f : r.faces) {
                            bvh->erase(f);
                            if (surface.live(f)) bvh->insert(surface, f);
                        }
                    }
                } else {
                    count_refusal(r.result, &stats);
                }
            }
        }

        // -- FLIP -------------------------------------------------------------
        if (settings.allow_flip) {
            gather_edges(surface, bvh, centre, query_radius, &edges);
            for (EdgeId e : edges) {
                if (budget <= 0) {
                    stats.hit_budget = true;
                    break;
                }
                if (!surface.live(e)) continue;
                const FlipResult r = flip_edge(surface, e, op, delta, /*force=*/false);
                if (r.result == TopologyResult::Ok) {
                    ++stats.flipped;
                    --budget;
                    feed_index(surface, bvh, r.faces, 2);
                } else {
                    // NoImprovement is the common answer and is not a refusal
                    // worth counting: the pass asks about every edge and flips
                    // the few that get better.
                    if (r.result != TopologyResult::NoImprovement) count_refusal(r.result, &stats);
                }
            }
        }

        // CONVERGED: a pass that changed nothing means the region already meets
        // the target, and running the remaining passes would only re-ask the
        // same questions.
        if (stats.total() == before) break;
    }

    if (settings.relax_after_remesh && settings.relax_strength > 0.0f)
        stats.relaxed =
            relax_region(surface, bvh, centre, radius, settings.relax_strength, settings, delta);

    return stats;
}

std::size_t relax_region(DynamicSurface& surface, DynamicBvh* bvh, kernel::cfloat3 centre,
                         float radius, float strength, const DynamicTopologySettings& settings,
                         TopologyDelta* delta) {
    if (radius <= 0.0f || strength <= 0.0f) return 0;
    const float r2 = radius * radius;
    const std::uint32_t blockers = blockers_for(settings);

    std::vector<FaceId> faces;
    if (bvh) {
        bvh->faces_in_ball(surface, centre, radius, &faces);
    } else {
        surface.faces().for_each_live([&](FaceId f, const DynamicFace&) { faces.push_back(f); });
    }

    // The vertices to move, IN SLOT ORDER and deduplicated.
    std::vector<VertexId> verts;
    for (FaceId f : faces) {
        VertexId v[3];
        if (!surface.face_vertices(f, v)) continue;
        for (int i = 0; i < 3; ++i)
            if (kernel::cdot2(surface.position_of(v[i]) - centre) <= r2) verts.push_back(v[i]);
    }
    std::sort(verts.begin(), verts.end(), [](VertexId a, VertexId b) { return a.slot < b.slot; });
    verts.erase(std::unique(verts.begin(), verts.end(),
                            [](VertexId a, VertexId b) { return a.slot == b.slot; }),
                verts.end());

    // SIMULTANEOUS, not sequential: every target is computed from the surface
    // as it is now, and only then are the positions written. A sweep that moved
    // each vertex as it went would give a result that depends on the slot order
    // — which is deterministic, and still wrong, because the answer would
    // change when an unrelated edit renumbered nothing at all.
    std::vector<kernel::cfloat3> targets(verts.size());
    std::vector<VertexId> ring;
    std::size_t moved = 0;
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const VertexId v = verts[i];
        targets[i] = surface.position_of(v);

        // A CONSTRAINED VERTEX DOES NOT SLIDE. Sliding one along the surface
        // moves the feature it defines: a boundary vertex slides off the
        // border, a seam vertex slides across the seam.
        bool constrained = false;
        std::vector<HalfEdgeId> fan;
        if (!surface.outgoing_halfedges(v, &fan)) continue;
        for (HalfEdgeId h : fan) {
            const EdgeId e = surface.edge_of(h);
            const DynamicEdge* rec = surface.edge(e);
            if (rec && (rec->constraints & blockers) != 0) constrained = true;
        }
        if (constrained) continue;

        if (!surface.one_ring(v, &ring) || ring.empty()) continue;
        kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
        for (VertexId n : ring) sum = sum + surface.position_of(n);
        const kernel::cfloat3 mean = sum / static_cast<float>(ring.size());

        // TANGENTIAL ONLY: the normal component is what would move the SURFACE,
        // and this pass is about spacing. Removing it is the whole difference
        // between a relax and a smooth.
        const DynamicVertex* rec = surface.vertex(v);
        if (!rec) continue;
        const kernel::cfloat3 to_mean = mean - rec->position;
        const kernel::cfloat3 n = rec->normal;
        const kernel::cfloat3 tangent = to_mean - n * kernel::cdot(to_mean, n);
        targets[i] = rec->position + tangent * strength;
    }

    std::vector<FaceId> touched;
    for (std::size_t i = 0; i < verts.size(); ++i) {
        DynamicVertex* rec = surface.vertex(verts[i]);
        if (!rec) continue;
        if (rec->position.x == targets[i].x && rec->position.y == targets[i].y &&
            rec->position.z == targets[i].z)
            continue;
        if (delta) delta->note_vertex(surface, verts[i]);
        rec->position = targets[i];
        if (delta) delta->sync_vertex(surface, verts[i]);
        ++moved;
        std::vector<FaceId> incident;
        if (surface.incident_faces(verts[i], &incident))
            touched.insert(touched.end(), incident.begin(), incident.end());
    }

    if (!touched.empty()) {
        std::sort(touched.begin(), touched.end(),
                  [](FaceId a, FaceId b) { return a.slot < b.slot; });
        touched.erase(std::unique(touched.begin(), touched.end(),
                                  [](FaceId a, FaceId b) { return a.slot == b.slot; }),
                      touched.end());
        surface.refresh_normals(touched);
        if (bvh) bvh->update_many(surface, touched);
        surface.bump_geometry();
    }
    return moved;
}

}  // namespace mesh
}  // namespace clay
