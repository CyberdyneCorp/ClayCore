#include "clay/mesh/multires_sculpt.h"

#include <algorithm>

namespace clay {
namespace mesh {

bool multires_offers(MeshBrush verb) {
    // Every verb, without exception. Worth stating rather than returning true
    // silently: `dynamic_offers` declines Layer, and the difference is
    // structural. Layer measures its ceiling from where the surface was when
    // the STROKE began, per vertex; on an adaptive surface half the vertices
    // under the brush at the end of a stroke did not exist at the start. Here
    // the topology at a level is fixed, so every vertex has that answer.
    (void)verb;
    return true;
}

// -- the record ---------------------------------------------------------------

void MultiresDelta::clear() {
    detail_.clear();
    base_vertices_.clear();
    base_before_.clear();
    base_after_.clear();
    detail_slot_.clear();
    base_slot_.clear();
}

std::size_t MultiresDelta::bytes() const {
    return detail_.capacity() * sizeof(DetailEntry) +
           base_vertices_.capacity() * sizeof(std::uint32_t) +
           (base_before_.capacity() + base_after_.capacity()) * sizeof(kernel::cfloat3) +
           detail_slot_.size() * (sizeof(std::uint64_t) + sizeof(std::uint32_t)) +
           base_slot_.size() * 2u * sizeof(std::uint32_t);
}

std::vector<std::uint32_t> MultiresDelta::levels() const {
    std::vector<std::uint32_t> out;
    if (!base_vertices_.empty()) out.push_back(0);
    for (const DetailEntry& e : detail_) out.push_back(e.level);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void MultiresDelta::note_detail(std::uint32_t level, std::uint32_t vertex,
                                const LocalDetail& before) {
    // The FIRST sighting wins: forty stamps of one stroke over one vertex is
    // one entry whose `before` is where the stroke found it.
    const std::uint64_t key = key_of(level, vertex);
    if (detail_slot_.find(key) != detail_slot_.end()) return;
    detail_slot_.emplace(key, static_cast<std::uint32_t>(detail_.size()));
    DetailEntry e;
    e.level = level;
    e.vertex = vertex;
    e.before = before;
    e.after = before;
    detail_.push_back(e);
}

void MultiresDelta::note_base(std::uint32_t vertex, kernel::cfloat3 before) {
    if (base_slot_.find(vertex) != base_slot_.end()) return;
    base_slot_.emplace(vertex, static_cast<std::uint32_t>(base_vertices_.size()));
    base_vertices_.push_back(vertex);
    base_before_.push_back(before);
    base_after_.push_back(before);
}

void MultiresDelta::sync_after(const MultiresSurface& surface) {
    for (DetailEntry& e : detail_)
        if (e.level < surface.level_count()) e.after = surface.detail_at(e.level).get(e.vertex);
    for (std::size_t i = 0; i < base_vertices_.size(); ++i)
        base_after_[i] = surface.base_position(base_vertices_[i]);
}

namespace {

bool record_matches(const MultiresSurface& surface, std::uint32_t level, std::uint32_t vertex) {
    return level < surface.level_count() && vertex < surface.topology_at(level).vertex_count;
}

}  // namespace

bool MultiresDelta::revert(MultiresSurface& surface) const {
    for (const DetailEntry& e : detail_)
        if (!record_matches(surface, e.level, e.vertex)) return false;
    for (std::uint32_t v : base_vertices_)
        if (v >= surface.base_vertex_count()) return false;
    for (std::size_t i = 0; i < base_vertices_.size(); ++i)
        surface.set_base_position(base_vertices_[i], base_before_[i]);
    for (const DetailEntry& e : detail_) surface.set_detail(e.level, e.vertex, e.before);
    return true;
}

bool MultiresDelta::apply(MultiresSurface& surface) const {
    for (const DetailEntry& e : detail_)
        if (!record_matches(surface, e.level, e.vertex)) return false;
    for (std::uint32_t v : base_vertices_)
        if (v >= surface.base_vertex_count()) return false;
    for (std::size_t i = 0; i < base_vertices_.size(); ++i)
        surface.set_base_position(base_vertices_[i], base_after_[i]);
    for (const DetailEntry& e : detail_) surface.set_detail(e.level, e.vertex, e.after);
    return true;
}

// -- the sculptor -------------------------------------------------------------

MultiresSculptor::MultiresSculptor(MultiresSurface& surface) : surface_(surface) {}
MultiresSculptor::~MultiresSculptor() = default;

void MultiresSculptor::begin_stroke() {
    level_deltas_.clear();
    // AND THE COARSE LEVELS' RECORDS, for the reason this call clears the
    // bound level's: `MeshBrush::Layer` measures its ceiling from where the
    // STROKE found the surface, and a coarse level under a crossing stroke has
    // exactly that question to answer about its own vertices.
    for (CoarseLevel& c : coarse_) c.deltas.clear();
}

void MultiresSculptor::set_automask_inputs(AutomaskInputs inputs) {
    automask_ = std::move(inputs);
    automask_set_ = true;
    if (sculptor_) sculptor_->set_automask_inputs(automask_);
}

void MultiresSculptor::set_defer_normals(bool defer) {
    defer_normals_ = defer;
    if (sculptor_) sculptor_->set_defer_normals(defer);
}

void MultiresSculptor::set_stage_telemetry(StageTelemetry* stages) {
    stages_ = stages;
    if (sculptor_) sculptor_->set_stage_telemetry(stages_);
}

void MultiresSculptor::set_telemetry(memory::PeakTelemetry* telemetry) {
    telemetry_ = telemetry;
    if (sculptor_) sculptor_->set_telemetry(telemetry_);
}

void MultiresSculptor::flush_normals() {
    // No binding is made just to flush: nothing has been deferred if nothing
    // has been stamped.
    if (sculptor_) sculptor_->flush_normals(&level_deltas_);
}

MeshSculptor* MultiresSculptor::level_sculptor() {
    bind();
    return sculptor_.get();
}

std::uint64_t MultiresSculptor::seed_revision() {
    if (!surface_.valid()) return kNoSeedRevision;
    bind();
    return sculptor_ ? sculptor_->seed_revision() : kNoSeedRevision;
}

void MultiresSculptor::bind() {
    const std::uint32_t level = surface_.sculpt_level();
    const std::uint64_t generation = surface_.cache_generation();
    // Rebound on a level change AND on a cache generation change. The second is
    // what makes this safe against a host that released the caches under memory
    // pressure while a sculptor existed: the level's `Mesh` is inside the cache,
    // so a stale `MeshSculptor` would hold a reference into storage that is
    // gone.
    if (sculptor_ && bound_level_ == level && bound_generation_ == generation) {
        // THE ONE THING THAT IS RE-READ ON A BINDING THAT IS STILL GOOD. The
        // faces this level does not store are positioned by the level BELOW,
        // and a stroke down there moves them without invalidating anything up
        // here — so the pointer is refreshed every time rather than only when
        // the sculptor is rebuilt.
        sculptor_->set_cross_level(&surface_.cross_level_at(level));
        return;
    }
    // THE ONE THING A GENERATION-ONLY REBIND KEEPS. `MeshBrush::Layer` measures
    // its ceiling from where the STROKE found the surface, so the records have
    // to outlive a cache drop the host made under memory pressure; only a LEVEL
    // change makes them meaningless, because they are indexed by a numbering
    // that changed with it.
    const bool same_level = bound_level_ == level;
    if (!same_level) level_deltas_.clear();

    Mesh& mesh = surface_.level_mesh(level);
    const Adjacency& adjacency = surface_.level_adjacency(level);
    sculptor_ = std::make_unique<MeshSculptor>(mesh, adjacency);
    bound_level_ = level;
    // THE SURFACE THIS LEVEL IS PART OF, which for a regionally refined
    // hierarchy is more than the level holds. Without it every walk inside the
    // sculptor reads the rim of the refined region as an open border of the
    // model; with it there is no level anywhere in the brush path, because the
    // neighbours it was missing simply have an identity. See `cross_level.h`.
    sculptor_->set_cross_level(&surface_.cross_level_at(level));
    if (automask_set_) sculptor_->set_automask_inputs(automask_);
    sculptor_->set_defer_normals(defer_normals_);
    sculptor_->set_telemetry(telemetry_);
    sculptor_->set_stage_telemetry(stages_);
    // THE HIERARCHY'S SPATIAL INDEX. Nothing picks against a level, so nothing
    // builds a ray tree for one, and without this every unseeded stamp resolves
    // its anchor by scanning the level — 0.49 ms at 100k level vertices against
    // 1.58 ms at 1M for the same 1k footprint, which is the model showing
    // through a fixed footprint. The chunk table is already there, per level,
    // with bounds and vertex lists; handing it over is what turns it into the
    // query the extreme-poly requirement asks for.
    //
    // Rebound with the sculptor, so a level or generation change cannot leave a
    // table describing a different mesh in the hands of a live sculptor.
    sculptor_->set_chunks(&surface_.level_chunks(level));
    // AND THE LEVELS BELOW THAT ARE PART OF THE SAME SURFACE. On a regionally
    // refined hierarchy the patches beside the refined region have no vertex
    // here at all, so a stamp reaching past the region has to write them where
    // they live. See `stamp`.
    bind_coarse(level, same_level);
    // Read AFTER every call above: any of them may have built a cache and moved
    // the generation on.
    bound_generation_ = surface_.cache_generation();
}

void MultiresSculptor::bind_coarse(std::uint32_t level, bool keep_records) {
    // THE SAME ASYMMETRY THE BOUND LEVEL'S RECORD KEEPS, and for the same
    // reason. The list itself is rebuilt every time -- it is derived from the
    // topology, and rebuilding it is what makes a rebind safe -- but a
    // generation-only rebind is a cache drop under a live stroke, and a coarse
    // level that lost its record would answer "where did the stroke find this
    // vertex" with the surface as it stands MID-STROKE. `MeshBrush::Layer`
    // would then deposit its whole ceiling again on the coarse side while the
    // bound level correctly held at it, which is a step at the seam that
    // appears only when the host is short of memory.
    std::vector<CoarseLevel> previous;
    if (keep_records) previous.swap(coarse_);
    coarse_.clear();
    for (std::uint32_t k = 0; k < level; ++k) {
        // THE OWNERSHIP RULE, and it is one line because the level above
        // already answers it. A level that refines every patch holds a vertex
        // point for every vertex of the level below, so nothing down there is
        // ever the vertex an artist is looking at -- which is every level of a
        // uniform-depth hierarchy, and why one takes this path at all.
        if (surface_.topology_at(k + 1).dense()) continue;
        CoarseLevel c;
        c.level = k;
        for (CoarseLevel& was : previous)
            if (was.level == k) c.deltas = std::move(was.deltas);
        coarse_.push_back(std::move(c));
    }
}

void build_multires_workset(const MeshSculptor& level_sculptor, std::uint32_t level,
                            SculptWorkset* out) {
    const SculptWorkset& src = level_sculptor.workset();
    const Adjacency& adjacency = level_sculptor.adjacency();

    // Retire the LAST stamp's marks through its own list, so the reset costs
    // what that stamp touched rather than what the level holds. Same rule the
    // two walks follow.
    for (WorkItemId item : out->items)
        if (item.key() < out->slot.size()) out->slot[item.key()] = kNoClass;
    const std::size_t vertices = level_sculptor.mesh().positions.size();
    if (out->slot.size() < vertices) out->slot.resize(vertices, kNoClass);

    const std::size_t n = src.size();
    out->items.resize(n);
    // `assign` rather than a copy-construct, so the member keeps the storage a
    // stroke already warmed.
    out->weights.assign(src.weights.begin(), src.weights.end());
    out->positions.assign(src.positions.begin(), src.positions.end());
    out->normals.assign(src.normals.begin(), src.normals.end());
    out->automask.assign(src.automask.begin(), src.automask.end());
    out->average_normal = src.average_normal;
    out->centroid = src.centroid;
    out->plane_point = src.plane_point;
    out->plane_normal = src.plane_normal;

    for (std::size_t i = 0; i < n; ++i) {
        std::size_t members = 0;
        const std::uint32_t v = adjacency.members(src.items[i].as_weld_class(), &members)[0];
        out->items[i] = WorkItemId::level_vertex(level, v);
        out->slot[v] = static_cast<std::uint32_t>(i);
    }

    out->write_region.clear();
    for (WorkItemId item : src.write_region) {
        std::size_t members = 0;
        const std::uint32_t v = adjacency.members(item.as_weld_class(), &members)[0];
        out->write_region.push_back(WorkItemId::level_vertex(level, v));
    }
    out->write_bounds = src.write_bounds;
}

const BrushScratchArena* MultiresSculptor::arena() const {
    return sculptor_ ? &sculptor_->arena() : nullptr;
}

namespace {

// The LEVEL VERTICES a write region names, through the adjacency's own member
// list rather than by assuming the two indices agree. They usually do -- a
// level's vertices are already distinct geometric points -- but two that
// coincide bit for bit weld into one class, and from that class onward every id
// would be off by one. The hierarchy stores detail per VERTEX, so an off-by-one
// here writes a wrinkle onto its neighbour.
void expand_write_region(const MeshSculptor& sculptor, std::vector<std::uint32_t>* out) {
    const Adjacency& adjacency = sculptor.adjacency();
    for (WorkItemId item : sculptor.write_region()) {
        std::size_t members = 0;
        const std::uint32_t* member = adjacency.members(item.as_weld_class(), &members);
        for (std::size_t i = 0; i < members; ++i) out->push_back(member[i]);
    }
}

// WHICH OF WHAT A COARSE STAMP MOVED IS THAT LEVEL'S TO KEEP. A vertex whose
// vertex point the level `above` stores is a vertex the artist is looking at up
// there, and the bound level's own write is the one that moves it -- so it goes
// back where it was rather than into a coefficient. That is exactly the
// partition `mixed_mesh_at_level` emits, asked through the same `ChildIndex`,
// which is what makes "no vertex of the mixed-depth surface is written twice" a
// property of the representation rather than a tolerance.
//
// Returns the number of weld classes with anything to keep.
std::size_t partition_coarse_write(const MeshSculptor& sculptor, ChildIndex above,
                                   std::vector<std::uint32_t>* owned,
                                   std::vector<std::uint32_t>* elsewhere) {
    const Adjacency& adjacency = sculptor.adjacency();
    std::size_t classes = 0;
    for (WorkItemId item : sculptor.write_region()) {
        std::size_t members = 0;
        const std::uint32_t* member = adjacency.members(item.as_weld_class(), &members);
        bool keep = false;
        for (std::size_t i = 0; i < members; ++i) {
            const bool mine = above.stored(member[i]) == kNoVertex;
            (mine ? owned : elsewhere)->push_back(member[i]);
            keep = keep || mine;
        }
        if (keep) ++classes;
    }
    return classes;
}

}  // namespace

// THE LAYER'S coefficients, not the base's: with an active layer the base is
// untouched, so recording it would produce an undo that restores something the
// gesture never changed and leaves the pass in place. The stack still holds the
// PRE-stamp values, because `absorb_level_edit` is the one call that writes
// them.
void MultiresSculptor::note_layer_before(std::uint32_t level,
                                         const std::vector<std::uint32_t>& vertices,
                                         SculptLayerId active_layer,
                                         SculptLayerDelta* layer_record) {
    layer_record->set_layer(active_layer);
    const DetailField* field = surface_.sculpt_layers().detail_at(active_layer, level);
    for (std::uint32_t v : vertices)
        layer_record->note_detail(level, v, field ? field->get(v) : LocalDetail{});
}

void MultiresSculptor::note_before(std::uint32_t level, const std::vector<std::uint32_t>& vertices,
                                   const VertexDeltas& deltas, SculptLayerId active_layer,
                                   MultiresDelta* record, SculptLayerDelta* layer_record) {
    if (active_layer != kNoSculptLayer) {
        if (layer_record != nullptr) note_layer_before(level, vertices, active_layer, layer_record);
        return;
    }
    if (record == nullptr) return;
    if (level > 0) {
        // The detail field still holds the PRE-stamp coefficients: nothing has
        // written it yet, because `absorb_level_edit` is the one call that does.
        const DetailField& detail = surface_.detail_at(level);
        for (std::uint32_t v : vertices) record->note_detail(level, v, detail.get(v));
        return;
    }
    for (std::uint32_t v : vertices) {
        // Where the STROKE found it, which the level record already keeps;
        // falling back to the current position would record a gesture that
        // undoes to the middle of itself.
        const std::optional<kernel::cfloat3> origin = deltas.origin_of(v);
        record->note_base(v, origin ? *origin : surface_.base_position(v));
    }
}

std::size_t MultiresSculptor::stamp_coarse(MeshBrush verb, const MeshBrushSettings& settings,
                                           const field::MaskGate& gate, SculptLayerId active_layer,
                                           MultiresDelta* record, SculptLayerDelta* layer_record) {
    std::size_t moved = 0;
    for (CoarseLevel& c : coarse_) {
        c.written.clear();
        not_owned_.clear();
        // BUILT FOR THE STAMP AND DROPPED WITH IT -- see `CoarseLevel` for the
        // memory that buys. The chunk table is handed over for the same reason
        // the bound level's is: without it an unseeded dab resolves its anchor
        // by scanning the level.
        //
        // `set_defer_normals` IS NOT FORWARDED, and the header says why: a
        // sculptor that lives for one stamp has no stroke to defer into, and a
        // deferred set dies unflushed with it. The telemetry blocks are not
        // forwarded either -- a peak reported here would be a second arena's
        // and not the bound level's, which `arena()` already says it reports.
        Mesh& mesh = surface_.level_mesh(c.level);
        MeshSculptor sculptor(mesh, surface_.level_adjacency(c.level));
        sculptor.set_cross_level(&surface_.cross_level_at(c.level));
        if (automask_set_) sculptor.set_automask_inputs(automask_);
        sculptor.set_chunks(&surface_.level_chunks(c.level));
        if (sculptor.stamp(verb, settings, gate, &c.deltas) == 0) continue;

        moved += partition_coarse_write(sculptor, ChildIndex::of(surface_.topology_at(c.level + 1)),
                                        &c.written, &not_owned_);
        surface_.restore_level_positions(c.level, not_owned_);
        if (c.written.empty()) continue;
        note_before(c.level, c.written, c.deltas, active_layer, record, layer_record);
        surface_.absorb_level_edit(c.level, c.written);
        write_levels_.push_back(c.level);
    }
    return moved;
}

// Rewrite every entry's "after" from the surface as it now is, so the last
// stamp of a gesture wins. One or the other and never both, because the active
// layer is one thing.
void MultiresSculptor::sync_after(SculptLayerId active_layer, MultiresDelta* record,
                                  SculptLayerDelta* layer_record) {
    if (active_layer != kNoSculptLayer) {
        if (layer_record) layer_record->sync_after(surface_.sculpt_layers());
        return;
    }
    if (record) record->sync_after(surface_);
}

// The vertices the bound level's stamp moved, and THE POSITIONS IT ASKED FOR --
// kept because the coarse writes move `S(n)` under this level and the level is
// re-evaluated from them. Absorbing this level first would store a coefficient
// that then reconstructs to the asked-for position PLUS that ripple, which is
// the doubled contribution at the seam the ordering exists to make impossible.
// See `stamp` in the header for the three measurements.
void MultiresSculptor::capture_fine_targets() {
    expand_write_region(*sculptor_, &touched_);
    fine_targets_.clear();
    const std::vector<kernel::cfloat3>& positions = sculptor_->mesh().positions;
    for (std::uint32_t v : touched_) fine_targets_.push_back(positions[v]);
}

std::size_t MultiresSculptor::stamp(MeshBrush verb, const MeshBrushSettings& settings,
                                    const field::MaskGate& gate, MultiresDelta* record,
                                    SculptLayerDelta* layer_record) {
    touched_.clear();
    write_levels_.clear();
    if (!surface_.valid()) return 0;
    // REFUSED BEFORE THE BRUSH MOVES ANYTHING. `absorb_level_edit` refuses a
    // locked layer too and puts the level's mesh back when it does, but that is
    // the belt for a direct caller; here the whole stamp is simply not taken,
    // so a locked layer costs a comparison rather than a gather and a restore.
    const SculptLayerId active_layer = surface_.sculpt_layers().active();
    const SculptLayer* layer = surface_.sculpt_layers().find(active_layer);
    if (layer && layer->locked) return 0;
    bind();
    const std::uint32_t level = bound_level_;

    // The level record is what `MeshBrush::Layer` measures its ceiling against
    // and what a level-0 gesture reads its "before" positions out of. It is
    // driven whether or not the caller wants a multires record, because Layer
    // needs it either way.
    const std::size_t moved = sculptor_->stamp(verb, settings, gate, &level_deltas_);
    if (moved != 0) capture_fine_targets();
    const std::size_t coarse =
        coarse_.empty() ? 0
                        : stamp_coarse(verb, settings, gate, active_layer, record, layer_record);
    if (moved == 0 && coarse == 0) return 0;

    // The hierarchy's own view of the stamp, before the level's weld classes are
    // expanded into the level vertices `absorb_level_edit` consumes.
    build_multires_workset(*sculptor_, level, &workset_);

    if (!touched_.empty()) {
        // Reading the mesh re-evaluates it from the levels the coarse writes
        // moved; the asked-for positions go back in on top of that, and the
        // coefficient the absorb stores is the one that reaches them from where
        // the surface now is.
        if (coarse != 0) {
            Mesh& mesh = surface_.level_mesh(level);
            for (std::size_t i = 0; i < touched_.size(); ++i)
                mesh.positions[touched_[i]] = fine_targets_[i];
        }
        note_before(level, touched_, level_deltas_, active_layer, record, layer_record);
        surface_.absorb_level_edit(level, touched_);
        write_levels_.push_back(level);
    }

    sync_after(active_layer, record, layer_record);
    return moved + coarse;
}

const std::vector<std::uint32_t>& MultiresSculptor::last_write_vertices_at(
    std::uint32_t level) const {
    static const std::vector<std::uint32_t> kNone;
    if (level == bound_level_) return touched_;
    for (const CoarseLevel& c : coarse_)
        if (c.level == level) return c.written;
    return kNone;
}

}  // namespace mesh
}  // namespace clay
