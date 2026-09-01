#include "clay/mesh/dynamic_sculpt.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace clay {
namespace mesh {

bool dynamic_offers(MeshBrush verb) {
    // See the header for why Layer is the one. Written as an exclusion rather
    // than an inclusion list so that a verb added later is offered by default
    // and has to be excluded on purpose.
    return verb != MeshBrush::Layer;
}

DynamicSculptor::DynamicSculptor(DynamicSurface& surface, const DynamicSculptOptions& options)
    : surface_(surface), options_(options) {
    bvh_.build(surface_, options_.index);
}

void DynamicSculptor::rebuild_index() { bvh_.build(surface_, options_.index); }

// -- the region ---------------------------------------------------------------

void DynamicSculptor::euclidean_region(kernel::cfloat3 centre, float radius) {
    region_vertices_.clear();
    region_distance_.clear();
    std::vector<FaceId> faces;
    bvh_.faces_in_ball(surface_, centre, radius, &faces);
    const float r2 = radius * radius;
    for (FaceId f : faces) {
        VertexId v[3];
        if (!surface_.face_vertices(f, v)) continue;
        for (int i = 0; i < 3; ++i) {
            if (v[i].slot >= slot_.size()) continue;
            if (slot_[v[i].slot] != kNoClass) continue;
            const float d2 = kernel::cdot2(surface_.position_of(v[i]) - centre);
            if (d2 > r2) continue;
            slot_[v[i].slot] = 0;  // provisional mark; the real index is set below
            region_vertices_.push_back(v[i]);
            region_distance_.push_back(std::sqrt(d2));
        }
    }
    // SORTED BY SLOT. The query returns faces in the tree's traversal order and
    // the tree's shape depends on the history of edits, so an unsorted region
    // would make a stamp's weighted normal — a float sum over this list — depend
    // on how the surface got here rather than on what it is.
    std::vector<std::uint32_t> order(region_vertices_.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return region_vertices_[a].slot < region_vertices_[b].slot;
    });
    std::vector<VertexId> sorted_v(order.size());
    std::vector<float> sorted_d(order.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        sorted_v[i] = region_vertices_[order[i]];
        sorted_d[i] = region_distance_[order[i]];
    }
    region_vertices_.swap(sorted_v);
    region_distance_.swap(sorted_d);
}

void DynamicSculptor::geodesic_region(kernel::cfloat3 centre, float radius, VertexId seed) {
    region_vertices_.clear();
    region_distance_.clear();
    if (!surface_.live(seed)) return;

    const std::size_t slots = surface_.vertices().capacity_slots();
    // RESIZE, NOT ASSIGN. The vertex pool grows on every stamp that splits, so
    // `assign` here re-wrote the whole array once per dab — O(surface) work on
    // the one path that must cost the footprint. `resize` fills only the slots
    // that are new, and every older slot is already `kUnreached` because the
    // walk retires its own marks through `walk_dirty_` before it returns.
    if (walk_distance_.size() < slots) walk_distance_.resize(slots, -1.0f);
    walk_dirty_.clear();
    walk_frontier_.clear();

    const float budget = radius * (options_.path_budget > 0.0f ? options_.path_budget : 1.0f);
    const float r2 = radius * radius;

    const float seed_d = kernel::clength(surface_.position_of(seed) - centre);
    if (seed_d > radius) return;
    walk_distance_[seed.slot] = seed_d;
    walk_dirty_.push_back(seed.slot);

    using Entry = std::pair<float, std::uint32_t>;
    const std::greater<Entry> later;
    walk_frontier_.emplace_back(seed_d, seed.slot);
    std::push_heap(walk_frontier_.begin(), walk_frontier_.end(), later);

    // An explicit heap over a reusable buffer, for the reason the fixed walk
    // records: a `std::priority_queue` owns its container, so one built here
    // would allocate on every dab.
    while (!walk_frontier_.empty()) {
        std::pop_heap(walk_frontier_.begin(), walk_frontier_.end(), later);
        const Entry top = walk_frontier_.back();
        walk_frontier_.pop_back();
        if (top.first > walk_distance_[top.second]) continue;  // a stale push

        const VertexId v = surface_.vertices().id_at(top.second);
        if (!v.valid()) continue;
        region_vertices_.push_back(v);
        region_distance_.push_back(top.first);

        const kernel::cfloat3 p = surface_.position_of(v);
        if (!surface_.one_ring(v, &ring_scratch_)) continue;
        for (VertexId n : ring_scratch_) {
            if (!surface_.live(n) || n.slot >= walk_distance_.size()) continue;
            const kernel::cfloat3 q = surface_.position_of(n);
            // (a) the path never leaves the ball...
            if (kernel::cdot2(q - centre) > r2) continue;
            // ...and (b) it stays inside the budget. That second condition is
            // the whole of "topological": the chin is inside the ball when the
            // brush is on the upper lip, and the only path to it runs the long
            // way round the mouth.
            const float d = top.first + kernel::clength(q - p);
            if (d > budget) continue;
            const float known = walk_distance_[n.slot];
            if (known >= 0.0f && known <= d) continue;
            if (known < 0.0f) walk_dirty_.push_back(n.slot);
            walk_distance_[n.slot] = d;
            walk_frontier_.emplace_back(d, n.slot);
            std::push_heap(walk_frontier_.begin(), walk_frontier_.end(), later);
        }
    }
    for (std::uint32_t s : walk_dirty_) walk_distance_[s] = -1.0f;
    walk_dirty_.clear();

    // Sorted by slot for the determinism reason above; the walk's own pop order
    // is a function of the geometry and is not the identity order.
    std::vector<std::uint32_t> order(region_vertices_.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return region_vertices_[a].slot < region_vertices_[b].slot;
    });
    std::vector<VertexId> sorted_v(order.size());
    std::vector<float> sorted_d(order.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        sorted_v[i] = region_vertices_[order[i]];
        sorted_d[i] = region_distance_[order[i]];
    }
    region_vertices_.swap(sorted_v);
    region_distance_.swap(sorted_d);
}

bool DynamicSculptor::gather(const MeshBrushSettings& brush, const field::MaskGate& gate,
                             bool geodesic) {
    const std::size_t slots = surface_.vertices().capacity_slots();
    if (slot_.size() < slots) {
        // Grown, not rewritten — same reason as the walk's distance array. The
        // slots that already existed still hold `kNoClass`, because the gather
        // below retires its own marks through the workset it kept.
        slot_.resize(slots, kNoClass);
        for (VertexId v : region_vertices_)
            if (v.slot < slot_.size()) slot_[v.slot] = kNoClass;
        region_vertices_.clear();
    } else {
        // Retire the LAST stamp's marks through its own list, so the reset costs
        // what that stamp touched rather than what the surface holds.
        for (VertexId v : region_vertices_)
            if (v.slot < slot_.size()) slot_[v.slot] = kNoClass;
    }

    if (geodesic) {
        VertexId seed;
        const DynamicBvh::ClosestPoint near = bvh_.closest(surface_, brush.center);
        if (near.found) {
            VertexId v[3];
            if (surface_.face_vertices(near.face, v)) {
                float best = 0.0f;
                for (int i = 0; i < 3; ++i) {
                    const float d = kernel::cdot2(surface_.position_of(v[i]) - brush.center);
                    if (!seed.valid() || d < best) {
                        seed = v[i];
                        best = d;
                    }
                }
            }
        }
        geodesic_region(brush.center, brush.radius, seed);
    } else {
        euclidean_region(brush.center, brush.radius);
    }
    if (region_vertices_.empty()) return false;

    const std::size_t n = region_vertices_.size();
    region_weights_.resize(n);
    region_positions_.resize(n);
    region_normals_.resize(n);

    // The alpha's frame, once for the stamp. Its fallback direction is the
    // normal nearest the centre, resolved lazily so a caller that named a
    // direction pays no query.
    AlphaFrame alpha_frame;
    if (brush.has_alpha()) {
        kernel::cfloat3 fallback = kernel::cf3(0, 1, 0);
        if (kernel::clength(brush.alpha_direction) < 1e-9f) {
            const DynamicBvh::ClosestPoint near = bvh_.closest(surface_, brush.center);
            if (near.found) fallback = surface_.face_normal(near.face);
        }
        alpha_frame = alpha_frame_for(brush, fallback);
    }

    // THE SAME WEIGHT COMPOSITION as the fixed path, in the same order, through
    // the same function. Anything else here would make the two representations
    // disagree about what a falloff means.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const VertexId v = region_vertices_[i];
        const DynamicVertex* rec = surface_.vertex(v);
        if (!rec) continue;
        const kernel::cfloat3 p = rec->position;

        WeightFactors f;
        f.falloff = falloff_weight(brush.falloff, kernel::clength(p - brush.center) / brush.radius);
        if (geodesic)
            f.path_taper = path_taper(region_distance_[i] / brush.radius, options_.taper_start,
                                      options_.path_budget);
        if (gate) f.gate = gate(p);
        // The surface's own painted mask rides on the vertex, so a converted
        // surface does not lose its freeze; it composes with the caller's gate
        // by taking whichever is stronger rather than multiplying, because two
        // independent freezes are not half a freeze each.
        f.gate = std::max(f.gate, rec->mask);
        f.alpha = alpha_at(brush, alpha_frame, p);
        const float w = compose_weight(f);
        if (w <= 0.0f) continue;

        region_vertices_[kept] = v;
        region_weights_[kept] = w;
        region_positions_[kept] = p;
        region_normals_[kept] = rec->normal;
        region_distance_[kept] = region_distance_[i];
        ++kept;
    }
    region_vertices_.resize(kept);
    region_weights_.resize(kept);
    region_positions_.resize(kept);
    region_normals_.resize(kept);
    region_distance_.resize(kept);
    if (kept == 0) return false;

    for (std::size_t i = 0; i < kept; ++i)
        slot_[region_vertices_[i].slot] = static_cast<std::uint32_t>(i);

    // The frame, from the snapshot and never from what the stamp is about to
    // deposit.
    kernel::cfloat3 nsum = kernel::cf3(0, 0, 0), psum = kernel::cf3(0, 0, 0);
    float wsum = 0.0f;
    for (std::size_t i = 0; i < kept; ++i) {
        nsum = nsum + region_normals_[i] * region_weights_[i];
        psum = psum + region_positions_[i] * region_weights_[i];
        wsum += region_weights_[i];
    }
    average_normal_ = safe_normalize(nsum, kernel::cf3(0, 1, 0));
    centroid_ = wsum > 0.0f ? psum / wsum : brush.center;
    if (brush.use_given_plane) {
        plane_point_ = brush.plane_point;
        plane_normal_ = safe_normalize(brush.plane_normal, average_normal_);
    } else {
        plane_point_ = centroid_;
        plane_normal_ = average_normal_;
    }
    return true;
}

void DynamicSculptor::build_neighbors(bool want_normals, bool want_colors) {
    static_assert(kNoClass == kOutsideRegion,
                  "a region slot is copied into the neighbour list unchanged");
    nb_offsets_.clear();
    nb_slots_.clear();
    nb_positions_.clear();
    nb_normals_.clear();
    nb_colors_.clear();
    nb_offsets_.push_back(0);
    for (VertexId v : region_vertices_) {
        if (surface_.one_ring(v, &ring_scratch_)) {
            for (VertexId n : ring_scratch_) {
                const DynamicVertex* rec = surface_.vertex(n);
                if (!rec) continue;
                nb_slots_.push_back(n.slot < slot_.size() ? slot_[n.slot] : kOutsideRegion);
                nb_positions_.push_back(rec->position);
                if (want_colors) nb_colors_.push_back(rec->color);
                if (want_normals) nb_normals_.push_back(rec->normal);
            }
        }
        nb_offsets_.push_back(static_cast<std::uint32_t>(nb_slots_.size()));
    }
}

SculptSnapshot DynamicSculptor::snapshot_of() const {
    SculptSnapshot s;
    s.positions = region_positions_.data();
    s.normals = region_normals_.data();
    s.weights = region_weights_.data();
    s.count = region_vertices_.size();
    s.average_normal = average_normal_;
    s.centroid = centroid_;
    s.plane_point = plane_point_;
    s.plane_normal = plane_normal_;
    return s;
}

SculptNeighbors DynamicSculptor::neighbors_of() const {
    SculptNeighbors n;
    n.offsets = nb_offsets_.data();
    n.slots = nb_slots_.data();
    n.positions = nb_positions_.data();
    n.normals = nb_normals_.empty() ? nullptr : nb_normals_.data();
    n.colors = nb_colors_.empty() ? nullptr : nb_colors_.data();
    return n;
}

std::size_t DynamicSculptor::write_positions(TopologyDelta* record) {
    std::size_t moved = 0;
    touched_faces_.clear();
    for (std::size_t i = 0; i < region_vertices_.size(); ++i) {
        if (is_zero(displacement_[i])) continue;
        const VertexId v = region_vertices_[i];
        DynamicVertex* rec = surface_.vertex(v);
        if (!rec) continue;
        if (record) record->note_vertex(surface_, v);
        rec->position = region_positions_[i] + displacement_[i];
        if (record) record->sync_vertex(surface_, v);
        ++moved;
        std::vector<FaceId> incident;
        if (surface_.incident_faces(v, &incident))
            touched_faces_.insert(touched_faces_.end(), incident.begin(), incident.end());
    }
    if (moved == 0) return 0;

    std::sort(touched_faces_.begin(), touched_faces_.end(),
              [](FaceId a, FaceId b) { return a.slot < b.slot; });
    touched_faces_.erase(std::unique(touched_faces_.begin(), touched_faces_.end(),
                                     [](FaceId a, FaceId b) { return a.slot == b.slot; }),
                         touched_faces_.end());
    // LOCAL normal recompute over the changed faces and every vertex they
    // touch, which is the ring — a face normal is shared, so a moved vertex
    // makes its neighbours' normals stale too.
    surface_.refresh_normals(touched_faces_);
    bvh_.update_many(surface_, touched_faces_);
    surface_.bump_geometry();
    return moved;
}

std::size_t DynamicSculptor::write_colors(TopologyDelta* record) {
    std::size_t painted = 0;
    for (std::size_t i = 0; i < region_vertices_.size(); ++i) {
        const VertexId v = region_vertices_[i];
        DynamicVertex* rec = surface_.vertex(v);
        if (!rec) continue;
        if (is_zero(color_target_[i] - rec->color)) continue;
        if (record) record->note_vertex(surface_, v);
        rec->color = color_target_[i];
        if (record) record->sync_vertex(surface_, v);
        ++painted;
    }
    if (painted) surface_.bump_attributes();
    return painted;
}

// -- the stamp ----------------------------------------------------------------

DynamicStampResult DynamicSculptor::stamp(MeshBrush verb, const MeshBrushSettings& brush,
                                          const DynamicTopologySettings& topology,
                                          const field::MaskGate& gate, TopologyDelta* record) {
    const DynamicStampResult out = stamp_impl(verb, brush, topology, gate, record);
    // ONE observation point for a call with several early returns. Splitting the
    // body out is what makes that possible without repeating the publish at
    // every `return`, where a later edit would eventually forget one.
    if (telemetry_ != nullptr) {
        telemetry_->observe_topology(out.remesh.total());
        // The adaptive surface's workset, which `MeshSculptor` cannot report
        // because this representation never builds one.
        telemetry_->observe_workset(region_vertices_.size());
    }
    return out;
}

DynamicStampResult DynamicSculptor::stamp_impl(MeshBrush verb, const MeshBrushSettings& brush,
                                               const DynamicTopologySettings& topology,
                                               const field::MaskGate& gate,
                                               TopologyDelta* record) {
    DynamicStampResult out;
    out.topology_revision = surface_.topology_revision();
    out.geometry_revision = surface_.geometry_revision();
    out.attribute_revision = surface_.attribute_revision();
    if (!dynamic_offers(verb) || brush.radius <= 0.0f) return out;

    const RemeshTiming timing = default_timing(verb);
    const bool before = topology.enabled && (timing == RemeshTiming::BeforeBrush ||
                                             timing == RemeshTiming::BeforeAndAfter);
    const bool after = topology.enabled && (timing == RemeshTiming::AfterBrush ||
                                            timing == RemeshTiming::BeforeAndAfter);

    if (before)
        out.remesh = remesh_region(surface_, &bvh_, brush.center, brush.radius, topology, record);

    const BrushModel model = model_of(verb);
    const BrushRuntimePlan plan = compile_plan(model, brush);
    if (!gather(brush, gate, brush.geodesic)) {
        if (after) {
            const RemeshStats late =
                remesh_region(surface_, &bvh_, brush.center, brush.radius, topology, record);
            out.remesh.split += late.split;
            out.remesh.collapsed += late.collapsed;
            out.remesh.flipped += late.flipped;
        }
        return out;
    }

    if (plan.needs_neighbors)
        build_neighbors(plan.needs_neighbor_normals, plan.needs_neighbor_colors);

    const SculptSnapshot snapshot = snapshot_of();
    const SculptNeighbors neighbors = plan.needs_neighbors ? neighbors_of() : SculptNeighbors{};

    if (plan.model.target == BrushWriteTarget::Color) {
        color_target_.resize(region_vertices_.size());
        for (std::size_t i = 0; i < region_vertices_.size(); ++i)
            color_target_[i] = surface_.vertices().at(region_vertices_[i]).color;
        color_current_.assign(color_target_.begin(), color_target_.end());
        if (verb == MeshBrush::Paint)
            kernel_paint(snapshot, brush, color_current_.data(), color_target_.data());
        else
            kernel_smear(snapshot, neighbors, brush, color_current_.data(), color_target_.data());
        out.moved_vertices = write_colors(record);
    } else {
        displacement_.assign(region_vertices_.size(), kernel::cf3(0, 0, 0));
        kernel::cfloat3* d = displacement_.data();
        // THE SHARED KERNELS. Not one line of deformation math lives in this
        // file, which is the property `add-shared-brush-kernels` exists to make
        // possible and the reason Clay means one thing.
        switch (verb) {
            case MeshBrush::Grab:
            case MeshBrush::Snakehook:
                kernel_grab(snapshot, brush, d);
                break;
            case MeshBrush::Draw:
                kernel_draw(snapshot, brush, d);
                break;
            case MeshBrush::Inflate:
                kernel_inflate(snapshot, brush, d);
                break;
            case MeshBrush::Pinch:
                kernel_pinch(snapshot, brush, d);
                break;
            case MeshBrush::Flatten:
                kernel_flatten(snapshot, brush, d);
                break;
            case MeshBrush::Clay:
                kernel_clay(snapshot, brush, d);
                break;
            case MeshBrush::Crease:
                kernel_crease(snapshot, brush, d);
                break;
            case MeshBrush::Nudge:
                kernel_nudge(snapshot, brush, d);
                break;
            case MeshBrush::Smooth:
            case MeshBrush::Polish:
            case MeshBrush::Scrape:
                kernel_smooth_family(verb, snapshot, neighbors, brush, scratch_, d);
                break;
            case MeshBrush::Relax:
                kernel_relax(snapshot, neighbors, brush, scratch_, d);
                break;
            case MeshBrush::Layer:
            case MeshBrush::Paint:
            case MeshBrush::Smear:
                break;  // handled above, or not offered
        }
        out.moved_vertices = write_positions(record);
    }

    for (std::size_t i = 0; i < region_vertices_.size(); ++i) {
        const DynamicVertex* rec = surface_.vertex(region_vertices_[i]);
        if (!rec) continue;
        out.dirty_bounds.expand(region_positions_[i]);
        out.dirty_bounds.expand(rec->position);
    }

    if (after) {
        const RemeshStats late =
            remesh_region(surface_, &bvh_, brush.center, brush.radius, topology, record);
        out.remesh.split += late.split;
        out.remesh.collapsed += late.collapsed;
        out.remesh.flipped += late.flipped;
        out.remesh.relaxed += late.relaxed;
    }

    out.topology_revision = surface_.topology_revision();
    out.geometry_revision = surface_.geometry_revision();
    out.attribute_revision = surface_.attribute_revision();
    return out;
}

}  // namespace mesh
}  // namespace clay
