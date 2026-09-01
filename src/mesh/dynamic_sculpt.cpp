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

// -- the topology adapter -----------------------------------------------------

namespace {

// THE ADAPTIVE SURFACE'S ANSWER TO `WorkItemTopology`.
//
// This is the whole of what the automask needed from this representation and
// did not have: two questions, both answered in workset slots, neither of them
// naming a `Mesh` or an `Adjacency`. With it, `compute_automask`'s neutral core
// runs here exactly as it runs on the fixed mesh, with the same fade, the same
// smoothstep and the same multiplication order — the only thing that moved
// behind the interface is how a ring is enumerated.
//
// It BORROWS its two walk buffers from the sculptor rather than owning them.
// One is constructed on the stack per stamp, so owning them would be an
// allocation per dab, which is the cost the change it belongs to exists to
// remove.
class DynamicSurfaceTopology final : public WorkItemTopology {
   public:
    DynamicSurfaceTopology(const DynamicSurface& surface, const SculptWorkset& workset,
                           std::vector<VertexId>* ring, std::vector<HalfEdgeId>* fan)
        : surface_(surface), workset_(workset), ring_(ring), fan_(fan) {}

    void ring_slots(std::uint32_t slot, ScratchVector<std::uint32_t>* out) const override {
        out->clear();
        if (!surface_.one_ring(workset_.items[slot].as_surface_vertex(), ring_, fan_)) return;
        for (VertexId n : *ring_) {
            if (n.slot >= workset_.slot.size()) continue;
            const std::uint32_t s = workset_.slot[n.slot];
            // Only neighbours THEMSELVES in the workset: both topological
            // factors spread over the workset alone, by construction.
            if (s == kNoClass) continue;
            out->push_back(s);
        }
    }

    bool on_open_border(std::uint32_t slot) const override {
        // `DynamicSurface::is_boundary_vertex`, inlined over the borrowed fan.
        // The member function builds a `std::vector<HalfEdgeId>` of its own on
        // every call that misses its one-comparison fast path, and this runs
        // once per workset entry.
        const VertexId v = workset_.items[slot].as_surface_vertex();
        const DynamicVertex* rec = surface_.vertex(v);
        if (!rec) return false;
        if (surface_.is_boundary_halfedge(rec->outgoing)) return true;
        if (!surface_.outgoing_halfedges(v, fan_)) return true;  // an open ring IS a border
        for (HalfEdgeId h : *fan_)
            if (surface_.is_boundary_halfedge(h)) return true;
        return false;
    }

   private:
    const DynamicSurface& surface_;
    const SculptWorkset& workset_;
    std::vector<VertexId>* ring_;
    std::vector<HalfEdgeId>* fan_;
};

}  // namespace

// -- the region ---------------------------------------------------------------

// SORTED BY SLOT, over arena scratch.
//
// The ball query returns faces in the tree's traversal order and the walk pops
// in an order that is a function of the geometry; the tree's shape depends on
// the history of edits. So an unsorted region would make a stamp's weighted
// normal — a float sum over this list — depend on how the surface got here
// rather than on what it is.
//
// The permutation and its two sorted copies were three `std::vector`s built
// inside this function, which is three allocations per dab on both region
// paths. They are the arena's now, and the scope hands them back before the
// composition asks it for anything.
void DynamicSculptor::sort_candidates_by_slot() {
    const std::size_t n = candidates_.size();
    if (n < 2) return;
    BrushArenaScope scope(arena_);
    std::uint32_t* order = arena_.allocate<std::uint32_t>(n);
    for (std::uint32_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order, order + n, [&](std::uint32_t a, std::uint32_t b) {
        return candidates_[a].slot < candidates_[b].slot;
    });
    VertexId* sorted_v = arena_.allocate<VertexId>(n);
    float* sorted_d = arena_.allocate<float>(n);
    for (std::size_t i = 0; i < n; ++i) {
        sorted_v[i] = candidates_[order[i]];
        sorted_d[i] = region_distance_[order[i]];
    }
    // Copied back rather than swapped in, so the members keep the storage a
    // stroke already warmed and the arena keeps its block.
    for (std::size_t i = 0; i < n; ++i) {
        candidates_[i] = sorted_v[i];
        region_distance_[i] = sorted_d[i];
    }
}

void DynamicSculptor::euclidean_region(kernel::cfloat3 centre, float radius) {
    candidates_.clear();
    region_distance_.clear();
    bvh_.faces_in_ball(surface_, centre, radius, &ball_faces_);
    const float r2 = radius * radius;
    std::vector<std::uint32_t>& slot = region_.slot;
    for (FaceId f : ball_faces_) {
        VertexId v[3];
        if (!surface_.face_vertices(f, v)) continue;
        for (int i = 0; i < 3; ++i) {
            if (v[i].slot >= slot.size()) continue;
            if (slot[v[i].slot] != kNoClass) continue;
            const float d2 = kernel::cdot2(surface_.position_of(v[i]) - centre);
            if (d2 > r2) continue;
            slot[v[i].slot] = 0;  // provisional mark; the real index is set below
            candidates_.push_back(v[i]);
            region_distance_.push_back(std::sqrt(d2));
        }
    }
    sort_candidates_by_slot();
}

void DynamicSculptor::geodesic_region(kernel::cfloat3 centre, float radius, VertexId seed) {
    candidates_.clear();
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
        candidates_.push_back(v);
        region_distance_.push_back(top.first);

        const kernel::cfloat3 p = surface_.position_of(v);
        if (!surface_.one_ring(v, &ring_scratch_, &fan_scratch_)) continue;
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

    sort_candidates_by_slot();
}

// -- what the composition cannot answer for itself ----------------------------

kernel::cfloat3 DynamicSculptor::normal_of_item(const void* context, WorkItemId item) {
    const DynamicSculptor* self = static_cast<const DynamicSculptor*>(context);
    const DynamicVertex* rec = self->surface_.vertex(item.as_surface_vertex());
    return rec ? rec->normal : kernel::cf3(0, 1, 0);
}

// The surface's own painted mask, which rides on the vertex so a converted
// surface does not lose its freeze. `compose_workset` takes whichever of this
// and the caller's gate is STRONGER rather than multiplying them, because two
// independent freezes are not half a freeze each.
float DynamicSculptor::mask_of_item(const void* context, WorkItemId item) {
    const DynamicSculptor* self = static_cast<const DynamicSculptor*>(context);
    const DynamicVertex* rec = self->surface_.vertex(item.as_surface_vertex());
    return rec ? rec->mask : 0.0f;
}

// The vertex of the surface nearest `p`: the corner of the closest face that is
// nearest it.
//
// THE ADAPTIVE COUNTERPART OF `MeshSculptor::nearest_class`, and it is written
// once because three callers want the same answer — the surface walk's seed,
// the connectivity automask's anchor, and the fallback facing the alpha and the
// normal-angle automask read. They used to answer it two different ways, one of
// them the nearest FACE's normal, which is a different estimator from the fixed
// path's nearest weld class and made the two representations disagree about a
// stamp for a reason nobody had decided on.
VertexId DynamicSculptor::nearest_vertex(kernel::cfloat3 p) const {
    VertexId best;
    const DynamicBvh::ClosestPoint near = bvh_.closest(surface_, p);
    if (!near.found) return best;
    VertexId v[3];
    if (!surface_.face_vertices(near.face, v)) return best;
    float best_d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d2 = kernel::cdot2(surface_.position_of(v[i]) - p);
        if (!best.valid() || d2 < best_d2) {
            best = v[i];
            best_d2 = d2;
        }
    }
    return best;
}

kernel::cfloat3 DynamicSculptor::automask_reference(const MeshBrushSettings& brush) {
    if (!is_zero(brush.deposit_normal))
        return safe_normalize(brush.deposit_normal, kernel::cf3(0, 1, 0));
    // The walk's own anchor when there is one, so the connectivity flood and
    // the normal-angle reference cannot disagree about where the brush landed
    // — the same rule the fixed sculptor states.
    if (const DynamicVertex* rec = surface_.vertex(automask_seed_)) return rec->normal;
    const DynamicVertex* near = surface_.vertex(nearest_vertex(brush.center));
    return near ? near->normal : kernel::cf3(0, 1, 0);
}

// -- the gather ---------------------------------------------------------------

void DynamicSculptor::build_dynamic_surface_workset(const MeshBrushSettings& brush,
                                                    bool geodesic) {
    SculptWorkset& r = region_;
    const std::size_t slots = surface_.vertices().capacity_slots();
    // Retire the LAST stamp's marks through its own list, so the reset costs
    // what that stamp touched rather than what the surface holds. Grown rather
    // than rewritten for the same reason the walk's distance array is: the pool
    // grows on every stamp that splits.
    for (WorkItemId item : r.items)
        if (item.key() < r.slot.size()) r.slot[item.key()] = kNoClass;
    r.items.clear();
    if (r.slot.size() < slots) r.slot.resize(slots, kNoClass);

    automask_seed_ = VertexId{};
    if (geodesic) {
        automask_seed_ = nearest_vertex(brush.center);
        geodesic_region(brush.center, brush.radius, automask_seed_);
    } else {
        // A ball query has no walk to have chosen an anchor, so the
        // connectivity automask's seed is resolved here — and ONLY when a
        // factor actually wants it, because it costs a query. The same rule the
        // fixed sculptor states.
        if (has_factor(brush.automask.factors, AutomaskFactor::TopologyConnected))
            automask_seed_ = nearest_vertex(brush.center);
        euclidean_region(brush.center, brush.radius);
    }

    // The walk speaks `VertexId`; the workset speaks `WorkItemId`. The
    // positions are lifted here because the neutral composition takes a
    // candidate's position as given — it is the one thing every representation
    // can hand over without being asked a question.
    r.items.resize(candidates_.size());
    r.positions.resize(candidates_.size());
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        r.items[i] = WorkItemId::surface_vertex(candidates_[i]);
        r.positions[i] = surface_.position_of(candidates_[i]);
    }
}

bool DynamicSculptor::gather(const MeshBrushSettings& brush, const field::MaskGate& gate,
                             bool geodesic) {
    // Every transient this stamp asks the arena for is dead when the stamp
    // ends, so the arena starts each one at zero and keeps its storage.
    arena_.reset();
    build_dynamic_surface_workset(brush, geodesic);
    if (region_.items.empty()) return false;

    // The alpha's frame, once for the stamp. Its fallback direction is the
    // normal nearest the centre, resolved lazily so a caller that named a
    // direction pays no query.
    AlphaFrame alpha_frame;
    if (brush.has_alpha()) {
        kernel::cfloat3 fallback = kernel::cf3(0, 1, 0);
        if (kernel::clength(brush.alpha_direction) < 1e-9f) {
            // THE NEAREST VERTEX'S NORMAL, not the nearest face's. The fixed
            // path takes the nearest weld class's angle-weighted normal, and
            // taking a face normal here made one stamp orient two different
            // ways on two representations for no reason anybody had chosen.
            const DynamicVertex* near = surface_.vertex(nearest_vertex(brush.center));
            if (near) fallback = near->normal;
        }
        alpha_frame = alpha_frame_for(brush, fallback);
    }

    // THE SAME COMPOSITION AS THE FIXED PATH, in the same order, through the
    // same function — including the automask, which this representation used to
    // drop on the floor. Anything else here would make the two disagree about
    // what a falloff means.
    const DynamicSurfaceTopology topology(surface_, region_, &ring_scratch_, &fan_scratch_);
    const WorkItemId seed_item = WorkItemId::surface_vertex(automask_seed_);

    WorkComposeInputs in;
    in.settings = &brush;
    in.gate = &gate;
    in.alpha = &alpha_frame;
    in.path_distance = region_distance_.data();
    in.geodesic = geodesic;
    in.taper_start = options_.taper_start;
    in.path_budget = options_.path_budget;
    in.topology = &topology;
    in.automask_inputs = &automask_inputs_;
    if (brush.automask.any()) in.automask_reference = automask_reference(brush);
    if (automask_seed_.valid()) in.automask_seed = &seed_item;
    in.reader.normal_at = &DynamicSculptor::normal_of_item;
    in.reader.mask_at = &DynamicSculptor::mask_of_item;
    in.reader.context = this;

    const std::size_t kept = compose_workset(in, arena_, &region_);
    if (kept == 0) return false;

    last_region_.resize(kept);
    for (std::size_t i = 0; i < kept; ++i)
        last_region_[i] = region_.items[i].as_surface_vertex();
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
    for (WorkItemId item : region_.items) {
        if (surface_.one_ring(item.as_surface_vertex(), &ring_scratch_, &fan_scratch_)) {
            for (VertexId n : ring_scratch_) {
                const DynamicVertex* rec = surface_.vertex(n);
                if (!rec) continue;
                nb_slots_.push_back(n.slot < region_.slot.size() ? region_.slot[n.slot]
                                                                 : kOutsideRegion);
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
    s.positions = region_.positions.data();
    s.normals = region_.normals.data();
    s.weights = region_.weights.data();
    s.count = region_.size();
    s.average_normal = region_.average_normal;
    s.centroid = region_.centroid;
    s.plane_point = region_.plane_point;
    s.plane_normal = region_.plane_normal;
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
    region_.write_region.clear();
    region_.write_bounds = math::Aabb{};
    for (std::size_t i = 0; i < region_.size(); ++i) {
        if (is_zero(displacement_[i])) continue;
        const VertexId v = region_.items[i].as_surface_vertex();
        DynamicVertex* rec = surface_.vertex(v);
        if (!rec) continue;
        if (record) record->note_vertex(surface_, v);
        rec->position = region_.positions[i] + displacement_[i];
        if (record) record->sync_vertex(surface_, v);
        ++moved;
        // THE WRITE REGION, not the workset: the rim of a falloff and a fully
        // masked vertex are gathered and never move.
        region_.write_region.push_back(region_.items[i]);
        region_.write_bounds.expand(region_.positions[i]);
        region_.write_bounds.expand(rec->position);
        // `incident_faces` INLINED OVER A BORROWED FAN. That call takes a
        // `std::vector<FaceId>*`, so the obvious spelling built one PER MOVED
        // VERTEX — an allocation per vertex rather than per stamp, which is the
        // worst shape a per-dab allocation can have.
        if (!surface_.outgoing_halfedges(v, &fan_scratch_)) continue;
        for (HalfEdgeId h : fan_scratch_) {
            const FaceId f = surface_.face_of(h);
            if (surface_.faces().live(f)) touched_faces_.push_back(f);
        }
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
    surface_.refresh_normals(touched_faces_, &normal_scratch_);
    bvh_.update_many(surface_, touched_faces_);
    surface_.bump_geometry();
    return moved;
}

std::size_t DynamicSculptor::write_colors(TopologyDelta* record) {
    std::size_t painted = 0;
    region_.write_region.clear();
    region_.write_bounds = math::Aabb{};
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const VertexId v = region_.items[i].as_surface_vertex();
        DynamicVertex* rec = surface_.vertex(v);
        if (!rec) continue;
        if (is_zero(color_target_[i] - rec->color)) continue;
        if (record) record->note_vertex(surface_, v);
        rec->color = color_target_[i];
        if (record) record->sync_vertex(surface_, v);
        region_.write_region.push_back(region_.items[i]);
        region_.write_bounds.expand(region_.positions[i]);
        ++painted;
    }
    if (painted) surface_.bump_attributes();
    return painted;
}

// -- the stamp ----------------------------------------------------------------

DynamicStampResult DynamicSculptor::stamp(MeshBrush verb, const MeshBrushSettings& brush,
                                          const DynamicTopologySettings& topology,
                                          const field::MaskGate& gate, TopologyDelta* record) {
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
        color_target_.resize(region_.size());
        for (std::size_t i = 0; i < region_.size(); ++i)
            color_target_[i] =
                surface_.vertices().at(region_.items[i].as_surface_vertex()).color;
        color_current_.assign(color_target_.begin(), color_target_.end());
        if (verb == MeshBrush::Paint)
            kernel_paint(snapshot, brush, color_current_.data(), color_target_.data());
        else
            kernel_smear(snapshot, neighbors, brush, color_current_.data(), color_target_.data());
        out.moved_vertices = write_colors(record);
    } else {
        displacement_.assign(region_.size(), kernel::cf3(0, 0, 0));
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

    for (std::size_t i = 0; i < region_.size(); ++i) {
        const DynamicVertex* rec = surface_.vertex(region_.items[i].as_surface_vertex());
        if (!rec) continue;
        out.dirty_bounds.expand(region_.positions[i]);
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
