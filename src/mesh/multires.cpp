#include "multires_internal.h"

#include <algorithm>
#include <cstring>

namespace clay {
namespace mesh {
namespace {

// Twelve bytes of coefficients, thirty-six of frame, twelve of subdivided
// position, twelve of position and twelve of display normal. Named here rather
// than spelled inline so `preflight_add_level` and `memory()` cannot drift into
// pricing the same thing differently.
constexpr std::uint64_t kDetailBytesPerVertex = sizeof(LocalDetail);
constexpr std::uint64_t kEvaluatedBytesPerVertex =
    sizeof(SurfaceFrame) + 3u * sizeof(kernel::cfloat3);
constexpr std::uint64_t kTopologyBytesPerFace = 4u * sizeof(std::uint32_t)   // corners
                                                + sizeof(std::uint32_t);     // patch id
// Edges plus the corner->edge map plus the two CSR incidences, per face and per
// vertex of the level. Approximate on purpose — it is a budget, not an
// accountant's figure, and it is measured exactly by `memory()` once the level
// exists.
constexpr std::uint64_t kConnBytesPerFace = 4u * sizeof(std::uint32_t) + 2u * sizeof(LevelEdge);
constexpr std::uint64_t kConnBytesPerVertex = 6u * sizeof(std::uint32_t);
// The level's own `mesh::Mesh` (quads and their triangulation) and its
// adjacency, both of which exist only for a level a brush is on.
constexpr std::uint64_t kRuntimeBytesPerFace = (4u + 6u) * sizeof(std::uint32_t);
constexpr std::uint64_t kRuntimeBytesPerVertex = 10u * sizeof(std::uint32_t);

}  // namespace

std::size_t LevelCache::bytes() const {
    std::size_t n = conn.bytes() + subdivided.capacity() * sizeof(kernel::cfloat3) +
                    frames.capacity() * sizeof(SurfaceFrame) + mesh.bytes();
    if (adjacency) {
        // The adjacency's own arrays are not exposed; its three CSR pairs are
        // close enough to ten words a vertex that pricing it any more precisely
        // would be false precision in a budget.
        n += static_cast<std::size_t>(mesh.positions.size()) * kRuntimeBytesPerVertex;
    }
    return n;
}

std::size_t AttrLevel::bytes() const {
    return topology.bytes() + conn.bytes() + to_geom.capacity() * sizeof(std::uint32_t) +
           uvs.capacity() * sizeof(kernel::cfloat2) + colors.capacity() * sizeof(kernel::cfloat3);
}

const char* multires_error_text(MultiresError error) {
    switch (error) {
        case MultiresError::None:
            return "no error";
        case MultiresError::EmptyBase:
            return "the cage has no faces, or an index count that is not a multiple of its arity";
        case MultiresError::IndexOutOfRange:
            return "a face index is past the end of the position array";
        case MultiresError::DegenerateFace:
            return "a face whose corners weld together has no area to subdivide";
        case MultiresError::NonManifold:
            return "three or more faces on one edge; the subdivision rules have no meaning there";
        case MultiresError::LevelOutOfRange:
            return "no such level";
        case MultiresError::NoLevelToRemove:
            return "the cage is the only level; there is nothing above it to remove";
        case MultiresError::OverBudget:
            return "the level's predicted cost exceeds the declared memory budget";
        case MultiresError::Cancelled:
            return "cancelled";
        case MultiresError::DetailPresent:
            return "the hierarchy carries detail; project onto a new cage instead of replacing it";
        case MultiresError::DepthLimit:
            return "the declared hierarchy is deeper than this build will reconstruct";
        case MultiresError::Decode:
            return "the buffer is truncated, corrupt, or from a newer writer";
    }
    return "unknown error";
}

MultiresSurface::MultiresSurface() : state_(nullptr) {}
MultiresSurface::~MultiresSurface() = default;
MultiresSurface::MultiresSurface(MultiresSurface&&) noexcept = default;
MultiresSurface& MultiresSurface::operator=(MultiresSurface&&) noexcept = default;

bool MultiresSurface::valid() const { return state_ != nullptr && !state_->levels.empty(); }

SubdivisionRule MultiresSurface::rule() const {
    static const MultiresOptions kDefaults;
    return state_ ? state_->options.rule : kDefaults.rule;
}

const MultiresOptions& MultiresSurface::options() const {
    static const MultiresOptions kDefaults;
    return state_ ? state_->options : kDefaults;
}

const Mesh& MultiresSurface::base_mesh() const {
    static const Mesh kEmpty;
    return state_ ? state_->base : kEmpty;
}

std::uint32_t MultiresSurface::level_count() const {
    return state_ ? static_cast<std::uint32_t>(state_->levels.size()) : 0u;
}

std::uint32_t MultiresSurface::max_level() const {
    const std::uint32_t n = level_count();
    return n == 0 ? 0u : n - 1u;
}

std::uint32_t MultiresSurface::sculpt_level() const { return state_ ? state_->sculpt_level : 0u; }
std::uint32_t MultiresSurface::display_level() const { return state_ ? state_->display_level : 0u; }

bool MultiresSurface::set_sculpt_level(std::uint32_t level) {
    if (!state_ || !state_->level_ok(level)) return false;
    state_->sculpt_level = level;
    return true;
}

bool MultiresSurface::set_display_level(std::uint32_t level) {
    if (!state_ || !state_->level_ok(level)) return false;
    state_->display_level = level;
    return true;
}

// -- construction -------------------------------------------------------------

namespace {

// The weld classes of a cage, as the three arrays everything below reads:
// which class each raw vertex is in, and which raw vertices each class holds.
void build_classes(MultiresSurface::State* s, const Adjacency& adj) {
    const std::size_t raw = s->base.positions.size();
    s->class_count = static_cast<std::uint32_t>(adj.class_count());
    s->class_of.resize(raw);
    for (std::size_t v = 0; v < raw; ++v) s->class_of[v] = adj.class_of(static_cast<std::uint32_t>(v));

    s->class_offsets.assign(static_cast<std::size_t>(s->class_count) + 1u, 0u);
    for (std::uint32_t c : s->class_of) s->class_offsets[c + 1]++;
    for (std::size_t i = 1; i < s->class_offsets.size(); ++i)
        s->class_offsets[i] += s->class_offsets[i - 1];
    s->class_members.assign(raw, 0u);
    std::vector<std::uint32_t> cursor(s->class_offsets);
    for (std::size_t v = 0; v < raw; ++v)
        s->class_members[cursor[s->class_of[v]]++] = static_cast<std::uint32_t>(v);

    s->attribute_split = s->class_count != static_cast<std::uint32_t>(raw);
}

}  // namespace

void gather_class_positions(const MultiresSurface::State& s, std::vector<kernel::cfloat3>* out) {
    out->assign(s.class_count, kernel::cf3(0, 0, 0));
    // The FIRST member of each class, which is the lowest raw index in it — a
    // choice by index rather than by value, so two coincident-within-epsilon
    // duplicates cannot make the cage's geometry depend on iteration order.
    for (std::uint32_t c = 0; c < s.class_count; ++c)
        (*out)[c] = s.base.positions[s.class_members[s.class_offsets[c]]];
}

std::optional<MultiresSurface> MultiresSurface::from_mesh(const Mesh& mesh,
                                                          const MultiresOptions& options,
                                                          MultiresError* out_error,
                                                          const parallel::CancelToken* cancel) {
    const auto fail = [&](MultiresError e) {
        if (out_error) *out_error = e;
        return std::optional<MultiresSurface>{};
    };
    if (out_error) *out_error = MultiresError::None;
    if (mesh.positions.empty() || mesh.indices.empty()) return fail(MultiresError::EmptyBase);

    auto s = std::make_unique<State>();
    s->options = options;
    s->base = mesh;

    const Adjacency adj = Adjacency::build(s->base, options.weld_epsilon);
    build_classes(s.get(), adj);

    MultiresLevel level0;
    if (!base_topology_from_mesh(s->base, s->class_of.data(), s->class_count, &level0.topology)) {
        // The three ways a cage is refused are told apart here rather than
        // collapsed into one, because a caller fixing a model needs to know
        // which one it hit.
        const bool quads = s->base.has_quads();
        const std::vector<std::uint32_t>& src = quads ? s->base.quads : s->base.indices;
        const std::uint32_t arity = quads ? 4u : 3u;
        if (src.empty() || src.size() % arity != 0) return fail(MultiresError::EmptyBase);
        for (std::uint32_t i : src)
            if (i >= s->base.positions.size()) return fail(MultiresError::IndexOutOfRange);
        return fail(MultiresError::DegenerateFace);
    }
    if (cancel && cancel->cancelled()) return fail(MultiresError::Cancelled);

    const LevelConnectivity conn = LevelConnectivity::build(level0.topology);
    if (conn.non_manifold) return fail(MultiresError::NonManifold);
    level0.edge_count = conn.edges.size();
    level0.pending_all = true;

    s->levels.push_back(std::move(level0));
    s->patch_dirty.assign(s->levels[0].topology.face_count, 0);
    s->base_frames_all = true;

    MultiresSurface surface;
    surface.state_ = std::move(s);
    return std::optional<MultiresSurface>(std::move(surface));
}

// -- pricing ------------------------------------------------------------------

MultiresPreflight MultiresSurface::preflight_add_level() const {
    MultiresPreflight p;
    if (!state_ || state_->levels.empty()) {
        p.allowed = false;
        p.error = MultiresError::EmptyBase;
        return p;
    }
    const MultiresLevel& parent = state_->levels.back();
    p.level = static_cast<std::uint32_t>(state_->levels.size());
    if (p.level >= kMaxLevels) {
        p.allowed = false;
        p.error = MultiresError::DepthLimit;
        return p;
    }

    // Arithmetic, not a build: a child's counts follow from the parent's, which
    // is why `MultiresLevel::edge_count` is kept.
    p.vertices = static_cast<std::uint64_t>(parent.topology.vertex_count) + parent.edge_count +
                 parent.topology.face_count;
    p.faces = parent.topology.corners.size();

    p.topology_bytes = p.faces * kTopologyBytesPerFace;
    p.detail_bytes = p.vertices * kDetailBytesPerVertex;
    p.evaluated_bytes = p.vertices * kEvaluatedBytesPerVertex;
    const std::uint64_t child_edges = 2ull * parent.edge_count + parent.topology.corners.size();
    p.runtime_bytes = p.faces * (kConnBytesPerFace + kRuntimeBytesPerFace) +
                      p.vertices * (kConnBytesPerVertex + kRuntimeBytesPerVertex);
    (void)child_edges;

    // What SURVIVES the call is the face list; the detail field starts empty
    // and grows only where the artist works. What the call itself needs on top
    // of that is the parent's connectivity, which is transient and is the
    // reason peak and persistent are reported apart.
    p.persistent_bytes = p.topology_bytes;
    const std::uint64_t parent_conn =
        static_cast<std::uint64_t>(parent.topology.face_count) * kConnBytesPerFace +
        static_cast<std::uint64_t>(parent.topology.vertex_count) * kConnBytesPerVertex;
    p.peak_bytes = p.persistent_bytes + parent_conn;

    if (p.vertices > kMaxLevelVertices) {
        p.allowed = false;
        p.error = MultiresError::OverBudget;
        return p;
    }
    if (state_->options.memory_budget != 0 && p.peak_bytes > state_->options.memory_budget) {
        p.allowed = false;
        p.error = MultiresError::OverBudget;
    }
    return p;
}

bool MultiresSurface::add_level(MultiresError* out_error, const parallel::CancelToken* cancel) {
    if (out_error) *out_error = MultiresError::None;
    if (!state_ || state_->levels.empty()) {
        if (out_error) *out_error = MultiresError::EmptyBase;
        return false;
    }
    const MultiresPreflight p = preflight_add_level();
    if (!p.allowed) {
        if (out_error) *out_error = p.error;
        return false;
    }

    // BUILD-THEN-PUBLISH. Everything below is assembled into locals; the
    // surface changes in the two statements at the end. A cancelled call
    // therefore leaves it byte-identical rather than one level into a state
    // nothing else knows how to read.
    const std::uint32_t parent_index = static_cast<std::uint32_t>(state_->levels.size() - 1);
    const LevelConnectivity& conn = connectivity_of(*state_, parent_index);
    if (cancel && cancel->cancelled()) {
        if (out_error) *out_error = MultiresError::Cancelled;
        return false;
    }

    MultiresLevel level;
    level.topology = subdivide_topology(state_->levels[parent_index].topology, conn);
    if (cancel && cancel->cancelled()) {
        if (out_error) *out_error = MultiresError::Cancelled;
        return false;
    }
    level.detail.reset(level.topology.vertex_count);
    level.edge_count = 2ull * state_->levels[parent_index].edge_count +
                       state_->levels[parent_index].topology.corners.size();
    level.pending_all = true;

    state_->levels.push_back(std::move(level));
    const std::uint32_t added = static_cast<std::uint32_t>(state_->levels.size() - 1);
    state_->sculpt_level = added;
    state_->display_level = added;
    return true;
}

bool MultiresSurface::remove_highest_level(MultiresError* out_error, DetailField* out_detail) {
    if (out_error) *out_error = MultiresError::None;
    if (!state_ || state_->levels.size() < 2) {
        if (out_error) *out_error = MultiresError::NoLevelToRemove;
        return false;
    }
    if (out_detail) *out_detail = std::move(state_->levels.back().detail);
    state_->levels.pop_back();
    state_->attr.resize(std::min<std::size_t>(state_->attr.size(), state_->levels.size()));
    const std::uint32_t top = max_level();
    state_->sculpt_level = std::min(state_->sculpt_level, top);
    state_->display_level = std::min(state_->display_level, top);
    ++state_->detail_revision;
    ++state_->evaluated_revision;
    return true;
}

bool MultiresSurface::set_base_mesh(const Mesh& mesh, MultiresError* out_error) {
    if (out_error) *out_error = MultiresError::None;
    if (!state_) {
        if (out_error) *out_error = MultiresError::EmptyBase;
        return false;
    }
    for (std::size_t l = 1; l < state_->levels.size(); ++l)
        if (!state_->levels[l].detail.empty()) {
            if (out_error) *out_error = MultiresError::DetailPresent;
            return false;
        }

    MultiresError err = MultiresError::None;
    std::optional<MultiresSurface> rebuilt = from_mesh(mesh, state_->options, &err);
    if (!rebuilt) {
        if (out_error) *out_error = err;
        return false;
    }
    const std::uint32_t levels = static_cast<std::uint32_t>(state_->levels.size());
    const std::uint32_t sculpt = state_->sculpt_level, display = state_->display_level;
    for (std::uint32_t l = 1; l < levels; ++l)
        if (!rebuilt->add_level(&err)) {
            if (out_error) *out_error = err;
            return false;
        }
    state_ = std::move(rebuilt->state_);
    state_->sculpt_level = std::min(sculpt, max_level());
    state_->display_level = std::min(display, max_level());
    ++state_->base_revision;
    ++state_->evaluated_revision;
    return true;
}

// -- detail -------------------------------------------------------------------

std::uint32_t MultiresSurface::base_vertex_count() const { return state_ ? state_->class_count : 0u; }

kernel::cfloat3 MultiresSurface::base_position(std::uint32_t vertex) const {
    if (!state_ || vertex >= state_->class_count) return kernel::cf3(0, 0, 0);
    return state_->base.positions[state_->class_members[state_->class_offsets[vertex]]];
}

const DetailField& MultiresSurface::detail_at(std::uint32_t level) const {
    static const DetailField kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    return state_->levels[level].detail;
}

DetailField& MultiresSurface::detail_mutable(std::uint32_t level) {
    static DetailField scratch;
    if (!state_ || !state_->level_ok(level)) return scratch;
    return state_->levels[level].detail;
}

std::uint64_t MultiresSurface::detail_checksum() const {
    std::uint64_t h = 0xcbf29ce484222325ull;
    if (!state_) return h;
    for (const MultiresLevel& l : state_->levels) {
        const std::uint64_t c = l.detail.checksum();
        for (int i = 0; i < 8; ++i) {
            h ^= (c >> (i * 8)) & 0xffull;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

// -- revisions and the dirty drain --------------------------------------------

std::uint64_t MultiresSurface::base_revision() const { return state_ ? state_->base_revision : 0; }
std::uint64_t MultiresSurface::detail_revision() const {
    return state_ ? state_->detail_revision : 0;
}
std::uint64_t MultiresSurface::evaluated_revision() const {
    return state_ ? state_->evaluated_revision : 0;
}

const std::vector<std::uint32_t>& MultiresSurface::dirty_patches() const {
    static const std::vector<std::uint32_t> kEmpty;
    return state_ ? state_->dirty_patches : kEmpty;
}

void MultiresSurface::clear_dirty() {
    if (!state_) return;
    // Reset through the LIST rather than clearing the whole mark array, so the
    // cost is what the host is draining and not what the cage holds.
    for (std::uint32_t p : state_->dirty_patches)
        if (p < state_->patch_dirty.size()) state_->patch_dirty[p] = 0;
    state_->dirty_patches.clear();
}

void mark_patches(MultiresSurface::State& s, std::uint32_t level,
                  const std::vector<std::uint32_t>& vertices) {
    if (!s.level_ok(level)) return;
    const LevelTopology& topology = s.levels[level].topology;
    const LevelConnectivity& conn = connectivity_of(s, level);
    if (s.patch_dirty.size() != s.levels[0].topology.face_count)
        s.patch_dirty.assign(s.levels[0].topology.face_count, 0);
    for (std::uint32_t v : vertices) {
        if (v >= topology.vertex_count) continue;
        std::size_t count = 0;
        const std::uint32_t* faces = conn.faces_of(v, &count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t patch = topology.patch_of(faces[i]);
            if (patch < s.patch_dirty.size() && !s.patch_dirty[patch]) {
                s.patch_dirty[patch] = 1;
                s.dirty_patches.push_back(patch);
            }
        }
    }
}

// -- residency ----------------------------------------------------------------

const MultiresEvalStats& MultiresSurface::eval_stats() const {
    static const MultiresEvalStats kEmpty;
    return state_ ? state_->stats : kEmpty;
}

void MultiresSurface::reset_eval_stats() {
    if (state_) state_->stats = MultiresEvalStats{};
}

std::uint64_t MultiresSurface::cache_generation() const {
    return state_ ? state_->cache_generation : 0;
}

MultiresMemory MultiresSurface::memory() const {
    MultiresMemory m;
    if (!state_) return m;
    m.base = state_->base.bytes() + state_->class_of.capacity() * sizeof(std::uint32_t) +
             state_->class_offsets.capacity() * sizeof(std::uint32_t) +
             state_->class_members.capacity() * sizeof(std::uint32_t);
    for (const MultiresLevel& l : state_->levels) {
        m.topology += l.topology.bytes();
        m.detail += l.detail.bytes();
        if (!l.cache) continue;
        ++m.resident_levels;
        m.evaluated += l.cache->subdivided.capacity() * sizeof(kernel::cfloat3) +
                       l.cache->frames.capacity() * sizeof(SurfaceFrame) +
                       l.cache->mesh.positions.capacity() * sizeof(kernel::cfloat3) +
                       l.cache->mesh.normals.capacity() * sizeof(kernel::cfloat3);
        m.runtime_index += l.cache->bytes() - l.cache->subdivided.capacity() * sizeof(kernel::cfloat3) -
                           l.cache->frames.capacity() * sizeof(SurfaceFrame) -
                           l.cache->mesh.positions.capacity() * sizeof(kernel::cfloat3) -
                           l.cache->mesh.normals.capacity() * sizeof(kernel::cfloat3);
    }
    for (const AttrLevel& a : state_->attr) m.runtime_index += a.bytes();
    m.authoritative = m.base + m.topology + m.detail;
    m.rebuildable = m.evaluated + m.runtime_index;
    m.total = m.authoritative + m.rebuildable;
    return m;
}

bool MultiresSurface::level_resident(std::uint32_t level) const {
    return state_ && state_->level_ok(level) && state_->levels[level].cache != nullptr;
}

void MultiresSurface::drop_all_caches() {
    if (!state_) return;
    for (MultiresLevel& l : state_->levels) {
        l.cache.reset();
        l.pending.clear();
        l.pending_all = true;
    }
    state_->attr.clear();
    state_->base_frames_all = true;
    state_->base_frames_dirty.clear();
}

void MultiresSurface::drop_inactive_caches() {
    if (!state_) return;
    // Evaluating a level reads its parent's positions, so the levels BELOW the
    // ones in use have to stay. What goes is everything above them, which on a
    // deep hierarchy is most of it.
    const std::uint32_t keep = std::max(state_->sculpt_level, state_->display_level);
    for (std::uint32_t l = keep + 1; l < state_->levels.size(); ++l) {
        state_->levels[l].cache.reset();
        state_->levels[l].pending.clear();
        state_->levels[l].pending_all = true;
    }
    state_->attr.resize(std::min<std::size_t>(state_->attr.size(), keep + 1u));
}

}  // namespace mesh
}  // namespace clay
