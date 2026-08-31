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

void MultiresSculptor::begin_stroke() { level_deltas_.clear(); }

void MultiresSculptor::set_automask_inputs(AutomaskInputs inputs) {
    automask_ = std::move(inputs);
    automask_set_ = true;
    if (sculptor_) sculptor_->set_automask_inputs(automask_);
}

void MultiresSculptor::set_defer_normals(bool defer) {
    defer_normals_ = defer;
    if (sculptor_) sculptor_->set_defer_normals(defer);
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

void MultiresSculptor::bind() {
    const std::uint32_t level = surface_.sculpt_level();
    const std::uint64_t generation = surface_.cache_generation();
    // Rebound on a level change AND on a cache generation change. The second is
    // what makes this safe against a host that released the caches under memory
    // pressure while a sculptor existed: the level's `Mesh` is inside the cache,
    // so a stale `MeshSculptor` would hold a reference into storage that is
    // gone.
    if (sculptor_ && bound_level_ == level && bound_generation_ == generation) return;
    if (bound_level_ != level) level_deltas_.clear();

    Mesh& mesh = surface_.level_mesh(level);
    const Adjacency& adjacency = surface_.level_adjacency(level);
    sculptor_ = std::make_unique<MeshSculptor>(mesh, adjacency);
    bound_level_ = level;
    // Read AFTER the two calls above: either of them may have built a cache and
    // moved the generation on.
    bound_generation_ = surface_.cache_generation();
    if (automask_set_) sculptor_->set_automask_inputs(automask_);
    sculptor_->set_defer_normals(defer_normals_);
}

std::size_t MultiresSculptor::stamp(MeshBrush verb, const MeshBrushSettings& settings,
                                    const field::MaskGate& gate, MultiresDelta* record) {
    touched_.clear();
    if (!surface_.valid()) return 0;
    bind();
    const std::uint32_t level = bound_level_;

    // The level record is what `MeshBrush::Layer` measures its ceiling against
    // and what a level-0 gesture reads its "before" positions out of. It is
    // driven whether or not the caller wants a multires record, because Layer
    // needs it either way.
    const std::size_t moved = sculptor_->stamp(verb, settings, gate, &level_deltas_);
    if (moved == 0) return 0;

    // WELD CLASSES BACK TO LEVEL VERTICES, through the adjacency's own member
    // list rather than by assuming the two indices agree. They usually do — a
    // level's vertices are already distinct geometric points — but two that
    // coincide bit for bit weld into one class, and from that class onward
    // every id would be off by one. The hierarchy stores detail per VERTEX, so
    // an off-by-one here writes a wrinkle onto its neighbour.
    const Adjacency& adjacency = sculptor_->adjacency();
    for (std::uint32_t cls : sculptor_->write_region()) {
        std::size_t member_count = 0;
        const std::uint32_t* members = adjacency.members(cls, &member_count);
        for (std::size_t i = 0; i < member_count; ++i) touched_.push_back(members[i]);
    }

    if (record) {
        if (level == 0) {
            for (std::uint32_t v : touched_) {
                // Where the STROKE found it, which the level record already
                // keeps; falling back to the current position would record a
                // gesture that undoes to the middle of itself.
                const std::optional<kernel::cfloat3> origin = level_deltas_.origin_of(v);
                record->note_base(v, origin ? *origin : surface_.base_position(v));
            }
        } else {
            // The detail field still holds the PRE-stamp coefficients: nothing
            // has written it yet, because `absorb_level_edit` below is the one
            // call that does.
            const DetailField& detail = surface_.detail_at(level);
            for (std::uint32_t v : touched_) record->note_detail(level, v, detail.get(v));
        }
    }

    surface_.absorb_level_edit(level, touched_);
    if (record) record->sync_after(surface_);
    return moved;
}

}  // namespace mesh
}  // namespace clay
