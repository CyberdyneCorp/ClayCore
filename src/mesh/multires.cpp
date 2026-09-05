#include <algorithm>
#include <cstring>

#include "clay/memory/capacity.h"

#include "multires_internal.h"

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

// What `std::vector` holds over what it was asked for.
//
// THE ESTIMATE MUST ERR HIGH, and before this factor it did not: the structural
// figures above are exact byte costs, and the arrays that carry them are grown
// rather than reserved, so a level measured after the fact holds its CAPACITY
// and not its size. Measured on a level 3 over a 64-quad cage: the evaluated
// arrays came out 1.34x their structural cost and the runtime ones 1.51x.
//
// A budget that errs LOW is the one that gets an application killed — it says
// yes to a level that does not fit, which is the single failure
// `preflight_add_level` exists to prevent — so the slack is applied to the
// prediction rather than left for the caller to remember. `test_multires.cpp`
// holds it as a CEILING over the measured actuals, not as a two-sided band.
constexpr double kCapacitySlack = 1.75;

}  // namespace

LevelCache::Bytes LevelCache::byte_split() const {
    Bytes b;
    b.evaluated = subdivided.capacity() * sizeof(kernel::cfloat3) +
                  frames.capacity() * sizeof(SurfaceFrame) +
                  mesh.positions.capacity() * sizeof(kernel::cfloat3) +
                  mesh.normals.capacity() * sizeof(kernel::cfloat3);
    // The level's own index buffers — its quads and their triangulation — plus
    // the connectivity, plus the adjacency when one has been built.
    b.runtime = conn.bytes() + mesh.indices.capacity() * sizeof(std::uint32_t) +
                mesh.quads.capacity() * sizeof(std::uint32_t) +
                mesh.colors.capacity() * sizeof(kernel::cfloat3) +
                mesh.uvs.capacity() * sizeof(kernel::cfloat2);
    b.chunk_index = chunks.bytes() + face_chunk.capacity() * sizeof(std::uint32_t);
    if (adjacency) {
        // The adjacency's own arrays are not exposed; its three CSR pairs are
        // close enough to ten words a vertex that pricing it any more precisely
        // would be false precision in a budget.
        b.runtime += static_cast<std::size_t>(mesh.positions.size()) * kRuntimeBytesPerVertex;
    }
    return b;
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
        case MultiresError::CapacityOverflow:
            return "the capacity estimate overflowed; the operation is refused rather than sized";
        case MultiresError::PatchNotRefinable:
            return "a patch, or a patch beside it, is not resident at the parent level; refining "
                   "it there would evaluate a border rule where the surface has no border";
        case MultiresError::NoPatchesRequested:
            return "a refinement over no patches; a level with no faces is not a level";
        case MultiresError::Decode:
            return "the buffer is truncated, corrupt, or from a newer writer";
        case MultiresError::NoSuchSculptLayer:
            return "no sculpt layer with that id on this hierarchy";
        case MultiresError::SculptLayerLocked:
            return "the sculpt layer is locked; unlock it or write to another channel";
        case MultiresError::SculptLayerStrokeOpen:
            return "a stroke is open; a composition change would author it against two surfaces";
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
    enforce_residency();
    return true;
}

bool MultiresSurface::set_display_level(std::uint32_t level) {
    if (!state_ || !state_->level_ok(level)) return false;
    state_->display_level = level;
    enforce_residency();
    return true;
}

void MultiresSurface::set_memory_profile(const memory::SculptMemoryProfile& profile) {
    if (!state_) return;
    state_->profile = profile;
    enforce_residency();
}

const memory::SculptMemoryProfile& MultiresSurface::memory_profile() const {
    static const memory::SculptMemoryProfile kDefault;
    return state_ ? state_->profile : kDefault;
}

void MultiresSurface::enforce_residency() {
    // ONE OF THE THREE MOMENTS EVICTION IS ALLOWED, and the one the host caused
    // itself: it just said which level it is sculpting or drawing. Nothing here
    // runs on a timer or on a high-water mark, because an engine that released a
    // cache the host did not ask about would be a second invalidation source the
    // host cannot predict — and `cache_generation` exists precisely because a
    // released cache is a use-after-free waiting for pressure to find it.
    //
    // On a constrained profile the SCULPT and DISPLAY levels stay resident and
    // everything else keeps its authoritative detail alone, which is the
    // residency rule the spec states. `drop_intermediate_caches` flushes pending
    // work first, so this drops a cache and never an edit.
    if (!state_ || !state_->profile.constrained()) return;
    const std::uint32_t resident =
        state_->profile.max_resident_levels == 0 ? 2u : state_->profile.max_resident_levels;
    // Two is what "the sculpt level and the display level" costs. A host that
    // asks for more gets the levels above the active ones released and the ones
    // between them kept, which is the cheaper of the two drops.
    if (resident <= 2)
        drop_intermediate_caches();
    else
        drop_inactive_caches();
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

    // THE INDICES ARE CHECKED FIRST, before anything is built over them. Not a
    // formality: `Adjacency::build` walks the index buffer, and a cage whose
    // indices point past its own vertices would be walked before
    // `base_topology_from_mesh` below ever got to refuse it.
    const bool quads = mesh.has_quads();
    const std::vector<std::uint32_t>& corners = quads ? mesh.quads : mesh.indices;
    const std::uint32_t arity = quads ? 4u : 3u;
    if (corners.empty() || corners.size() % arity != 0) return fail(MultiresError::EmptyBase);
    for (std::uint32_t i : mesh.indices)
        if (i >= mesh.positions.size()) return fail(MultiresError::IndexOutOfRange);
    for (std::uint32_t i : corners)
        if (i >= mesh.positions.size()) return fail(MultiresError::IndexOutOfRange);

    auto s = std::make_unique<State>();
    s->options = options;
    s->base = mesh;

    const Adjacency adj = Adjacency::build(s->base, options.weld_epsilon);
    build_classes(s.get(), adj);

    MultiresLevel level0;
    if (!base_topology_from_mesh(s->base, s->class_of.data(), s->class_count, &level0.topology)) {
        // Everything else `base_topology_from_mesh` refuses for was ruled out
        // above, so what is left is a face whose corners weld together.
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
    sync_stack_levels(*s);

    MultiresSurface surface;
    surface.state_ = std::move(s);
    return std::optional<MultiresSurface>(std::move(surface));
}

// -- pricing ------------------------------------------------------------------

namespace {

// The pricing every `preflight_*` shares, over counts its caller has already
// worked out. Split out when regional levels arrived: the counts differ, the
// price of a vertex and a face does not, and two copies of this arithmetic
// would be two answers to "what does a level cost".
MultiresPreflight price_level(const MultiresSurface::State& s, const MultiresLevel& parent,
                              std::uint32_t level, std::uint64_t vertices, std::uint64_t faces) {
    MultiresPreflight p;
    p.level = level;
    p.vertices = vertices;
    p.faces = faces;

    const auto with_slack = [](std::uint64_t exact) {
        return static_cast<std::uint64_t>(static_cast<double>(exact) * kCapacitySlack);
    };
    memory::CapacityBuilder topology_cost;
    // The face list is ONE exactly-sized allocation, so it needs no slack.
    topology_cost.authoritative(p.faces, kTopologyBytesPerFace);
    p.topology_bytes = topology_cost.finish().persistent_bytes;
    // FULLY detailed means DENSE, and a promoted field's payload is one exact
    // allocation — so the coefficients are exact and only the block table it
    // leaves behind is grown. Two `uint32` vectors over the blocks, at the
    // doubling ceiling.
    {
        memory::CapacityBuilder detail_cost;
        const std::uint64_t blocks =
            (p.vertices + DetailField::kDefaultBlockSize - 1) / DetailField::kDefaultBlockSize;
        detail_cost.authoritative(p.vertices, kDetailBytesPerVertex);
        detail_cost.authoritative(blocks, 4u * 2u * 2u);
        p.detail_bytes = detail_cost.finish().persistent_bytes;
    }

    memory::CapacityBuilder cost;
    // Persistent: the face list. What the call NEEDS on top of it is the
    // parent's connectivity, which is transient and is the reason peak and
    // persistent are reported apart.
    cost.authoritative(p.faces, kTopologyBytesPerFace);
    cost.transient(parent.topology.face_count, kConnBytesPerFace);
    cost.transient(parent.topology.vertex_count, kConnBytesPerVertex);
    const memory::CapacityEstimate estimate = cost.finish(s.options.memory_budget);

    memory::CapacityBuilder evaluated_cost;
    evaluated_cost.authoritative(p.vertices, kEvaluatedBytesPerVertex);
    p.evaluated_bytes = with_slack(evaluated_cost.finish().persistent_bytes);
    memory::CapacityBuilder runtime_cost;
    runtime_cost.authoritative(p.faces, kConnBytesPerFace + kRuntimeBytesPerFace);
    runtime_cost.authoritative(p.vertices, kConnBytesPerVertex + kRuntimeBytesPerVertex);
    p.runtime_bytes = with_slack(runtime_cost.finish().persistent_bytes);

    p.persistent_bytes = estimate.persistent_bytes;
    p.peak_bytes = estimate.peak_bytes;
    if (estimate.error == memory::BudgetError::Overflow) {
        p.allowed = false;
        p.error = MultiresError::CapacityOverflow;
        return p;
    }

    if (p.vertices > MultiresSurface::kMaxLevelVertices) {
        p.allowed = false;
        p.error = MultiresError::OverBudget;
        return p;
    }
    if (!estimate.allowed) {
        p.allowed = false;
        p.error = MultiresError::OverBudget;
    }
    return p;
}

}  // namespace

MultiresPreflight MultiresSurface::preflight_add_level() const {
    MultiresPreflight p;
    if (!state_ || state_->levels.empty()) {
        p.allowed = false;
        p.error = MultiresError::EmptyBase;
        return p;
    }
    const MultiresLevel& parent = state_->levels.back();
    const std::uint32_t level = static_cast<std::uint32_t>(state_->levels.size());
    if (level >= kMaxLevels) {
        p.level = level;
        p.allowed = false;
        p.error = MultiresError::DepthLimit;
        return p;
    }

    // Arithmetic, not a build: a child's counts follow from the parent's, which
    // is why `MultiresLevel::edge_count` is kept.
    //
    // CHECKED arithmetic, through the one estimator every priced operation in
    // this library now shares. A level's vertex count is quadratic in the
    // depth, so `vertices * bytes_per_vertex` is exactly the multiply that
    // wraps on a hostile or merely ambitious hierarchy — and the failure mode
    // of a wrapped estimate is that the level is ALLOWED, which is the one
    // outcome this call exists to prevent. An overflow is reported as a
    // refusal.
    const std::uint64_t vertices = static_cast<std::uint64_t>(parent.topology.vertex_count) +
                                   parent.edge_count + parent.topology.face_count;
    return price_level(*state_, parent, level, vertices, parent.topology.corners.size());
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
    // Every layer gains a slot for the new level, sized lazily on first write.
    // A layer over a twelve-level hierarchy costs the levels it REACHED.
    sync_stack_levels(*state_);
    const std::uint32_t added = static_cast<std::uint32_t>(state_->levels.size() - 1);
    state_->sculpt_level = added;
    state_->display_level = added;
    return true;
}

// -- regional levels ----------------------------------------------------------

std::uint32_t patch_total(const MultiresSurface::State& s) {
    return s.levels.empty() ? 0u : s.levels[0].topology.face_count;
}

namespace {

// Every face incident to a vertex is a neighbour of every other face incident to
// it, gathered per patch and then flattened. Sorted and deduplicated, so a ring
// is in ascending patch order and a dilation over it is deterministic.
void build_patch_rings(MultiresSurface::State& s, std::uint32_t patches) {
    const LevelTopology& base = s.levels[0].topology;
    const LevelConnectivity& conn = connectivity_of(s, 0);
    std::vector<std::vector<std::uint32_t>> ring(patches);
    for (std::uint32_t v = 0; v < base.vertex_count; ++v) {
        std::size_t n = 0;
        const std::uint32_t* faces = conn.faces_of(v, &n);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t k = 0; k < n; ++k)
                if (i != k) ring[faces[i]].push_back(faces[k]);
    }
    s.patch_ring_offsets.assign(patches + 1u, 0u);
    for (std::uint32_t f = 0; f < patches; ++f) {
        std::vector<std::uint32_t>& r = ring[f];
        std::sort(r.begin(), r.end());
        r.erase(std::unique(r.begin(), r.end()), r.end());
        s.patch_ring_offsets[f + 1] =
            s.patch_ring_offsets[f] + static_cast<std::uint32_t>(r.size());
        s.patch_ring.insert(s.patch_ring.end(), r.begin(), r.end());
    }
}

}  // namespace

const std::uint32_t* patch_neighbours(MultiresSurface::State& s, std::uint32_t patch,
                                      std::size_t* count) {
    const std::uint32_t patches = patch_total(s);
    if (s.patch_ring_offsets.empty() && patches != 0) build_patch_rings(s, patches);
    if (patch + 1u >= s.patch_ring_offsets.size()) {
        *count = 0;
        return nullptr;
    }
    const std::uint32_t begin = s.patch_ring_offsets[patch];
    *count = s.patch_ring_offsets[patch + 1] - begin;
    return s.patch_ring.data() + begin;
}

namespace {

// The requested patches as a per-patch flag, refusing a request this hierarchy
// cannot answer exactly.
//
// EXACTLY, and that is the whole check: a patch may be refined only where its
// stencils see the same parent neighbourhood the dense level would have seen,
// which is itself and its vertex ring resident one level down. Anything less
// evaluates Catmull-Clark's BORDER rule at an edge that is not a border, and
// the fine patch walks away from the coarse one it is meant to meet.
bool resolve_keep(MultiresSurface::State& s, const std::vector<std::uint32_t>& patches,
                  std::vector<char>* out, MultiresError* out_error) {
    const std::uint32_t patches_total = patch_total(s);
    const MultiresLevel& parent = s.levels.back();
    out->assign(patches_total, 0);
    std::uint32_t named = 0;
    for (std::uint32_t p : patches) {
        if (p >= patches_total || (*out)[p]) continue;
        if (!parent.keeps(p)) {
            if (out_error) *out_error = MultiresError::PatchNotRefinable;
            return false;
        }
        std::size_t n = 0;
        const std::uint32_t* ring = patch_neighbours(s, p, &n);
        for (std::size_t i = 0; i < n; ++i) {
            if (parent.keeps(ring[i])) continue;
            if (out_error) *out_error = MultiresError::PatchNotRefinable;
            return false;
        }
        (*out)[p] = 1;
        ++named;
    }
    if (named == 0) {
        if (out_error) *out_error = MultiresError::NoPatchesRequested;
        return false;
    }
    // A request naming every patch IS the dense level, and is stored as one:
    // an all-true `patch_kept` would make every level of an ordinary hierarchy
    // carry a byte per patch to say nothing.
    if (named == patches_total) out->clear();
    return true;
}

// An upper bound on what a regional level costs, without building it.
//
// Each kept parent face of arity `a` contributes one face point, `a` edge
// points and `a` vertex points, and the shared ones are counted once per face
// that shares them. So this OVER-counts, which is the direction a budget has to
// err in: the failure mode of an estimate that errs low is a level that is
// allowed and does not fit.
std::uint64_t regional_vertex_bound(const LevelTopology& parent, const std::vector<char>& keep,
                                    std::uint64_t* out_faces) {
    std::uint64_t vertices = 0, faces = 0;
    for (std::uint32_t f = 0; f < parent.face_count; ++f) {
        const std::uint32_t patch = parent.patch_of(f);
        if (!keep.empty() && (patch >= keep.size() || !keep[patch])) continue;
        const std::uint64_t arity = parent.face_arity(f);
        vertices += 1u + 2u * arity;
        faces += arity;
    }
    *out_faces = faces;
    return vertices;
}

}  // namespace

MultiresPreflight MultiresSurface::preflight_add_level_for_patches(
    const std::vector<std::uint32_t>& patches) const {
    MultiresPreflight p;
    if (!state_ || state_->levels.empty()) {
        p.allowed = false;
        p.error = MultiresError::EmptyBase;
        return p;
    }
    const std::uint32_t level = static_cast<std::uint32_t>(state_->levels.size());
    if (level >= kMaxLevels) {
        p.level = level;
        p.allowed = false;
        p.error = MultiresError::DepthLimit;
        return p;
    }
    std::vector<char> keep;
    MultiresError err = MultiresError::None;
    if (!resolve_keep(*state_, patches, &keep, &err)) {
        p.level = level;
        p.allowed = false;
        p.error = err;
        return p;
    }
    const MultiresLevel& parent = state_->levels.back();
    std::uint64_t faces = 0;
    const std::uint64_t vertices = regional_vertex_bound(parent.topology, keep, &faces);
    return price_level(*state_, parent, level, vertices, faces);
}

bool MultiresSurface::add_level_for_patches(const std::vector<std::uint32_t>& patches,
                                            MultiresError* out_error,
                                            const parallel::CancelToken* cancel) {
    if (out_error) *out_error = MultiresError::None;
    if (!state_ || state_->levels.empty()) {
        if (out_error) *out_error = MultiresError::EmptyBase;
        return false;
    }
    std::vector<char> keep;
    if (!resolve_keep(*state_, patches, &keep, out_error)) return false;
    // The DENSE path when every patch is named, so an all-patches regional
    // refine is not merely equal to a global one -- it is the same code.
    if (keep.empty()) return add_level(out_error, cancel);

    const MultiresPreflight p = preflight_add_level_for_patches(patches);
    if (!p.allowed) {
        if (out_error) *out_error = p.error;
        return false;
    }

    // BUILD-THEN-PUBLISH, exactly as `add_level`: nothing below touches the
    // surface until the last three statements.
    const std::uint32_t parent_index = static_cast<std::uint32_t>(state_->levels.size() - 1);
    const LevelConnectivity& conn = connectivity_of(*state_, parent_index);
    if (cancel && cancel->cancelled()) {
        if (out_error) *out_error = MultiresError::Cancelled;
        return false;
    }

    MultiresLevel level;
    level.topology =
        subdivide_topology_for_patches(state_->levels[parent_index].topology, conn, keep);
    if (cancel && cancel->cancelled()) {
        if (out_error) *out_error = MultiresError::Cancelled;
        return false;
    }
    level.detail.reset(level.topology.vertex_count);
    // A BOUND rather than the dense recurrence, which does not hold here: this
    // level's faces are a subset and its edges are not `2E + C`. Four per quad
    // counts every shared edge twice, so the next level's preflight errs high
    // -- the direction the header requires.
    level.edge_count = 4ull * level.topology.face_count;
    level.pending_all = true;
    level.patch_kept = std::move(keep);

    state_->levels.push_back(std::move(level));
    sync_stack_levels(*state_);
    const std::uint32_t added = static_cast<std::uint32_t>(state_->levels.size() - 1);
    state_->sculpt_level = added;
    state_->display_level = added;
    return true;
}

bool MultiresSurface::refine_patches_to_level(const std::vector<std::uint32_t>& patches,
                                              std::uint32_t target_level,
                                              MultiresError* out_error,
                                              const parallel::CancelToken* cancel) {
    if (out_error) *out_error = MultiresError::None;
    if (!state_ || state_->levels.empty()) {
        if (out_error) *out_error = MultiresError::EmptyBase;
        return false;
    }
    if (target_level == 0) return true;
    if (target_level >= kMaxLevels) {
        if (out_error) *out_error = MultiresError::DepthLimit;
        return false;
    }

    // Level `target - k` is refined over `patches` grown by `k` rings, so every
    // level above it evaluates its stencils against a parent that is complete
    // where it needs to be. Grown from the top down into `sets`, which makes
    // the growth one pass rather than one per level.
    // The guard FIRST: a hierarchy already at or past the target has nothing to
    // build, and the subtraction below would wrap rather than say so.
    const std::uint32_t have = static_cast<std::uint32_t>(state_->levels.size());
    if (have > target_level) return true;
    const std::uint32_t depth = target_level - have + 1;

    std::vector<std::vector<std::uint32_t>> sets(depth);
    std::vector<char> seen(patch_total(*state_), 0);
    std::vector<std::uint32_t> current;
    for (std::uint32_t p : patches) {
        if (p >= seen.size() || seen[p]) continue;
        seen[p] = 1;
        current.push_back(p);
    }
    std::sort(current.begin(), current.end());
    for (std::uint32_t k = 0; k < depth; ++k) {
        sets[depth - 1u - k] = current;
        if (k + 1u == depth) break;
        // One ring wider. Ascending, because the frontier is ascending and each
        // patch's own ring is -- so the same request twice grows the same set.
        std::vector<std::uint32_t> grown = current;
        for (std::uint32_t at : current) {
            std::size_t n = 0;
            const std::uint32_t* ring = patch_neighbours(*state_, at, &n);
            for (std::size_t i = 0; i < n; ++i)
                if (!seen[ring[i]]) {
                    seen[ring[i]] = 1;
                    grown.push_back(ring[i]);
                }
        }
        std::sort(grown.begin(), grown.end());
        current = std::move(grown);
    }

    for (const std::vector<std::uint32_t>& set : sets)
        if (!add_level_for_patches(set, out_error, cancel)) return false;
    return true;
}

std::uint32_t MultiresSurface::patch_max_level(std::uint32_t patch) const {
    if (!state_ || state_->levels.empty()) return 0;
    for (std::uint32_t l = static_cast<std::uint32_t>(state_->levels.size()); l-- > 1;)
        if (state_->levels[l].keeps(patch)) return l;
    return 0;
}

std::uint32_t MultiresSurface::effective_level(std::uint32_t patch, std::uint32_t level) const {
    return std::min(level, patch_max_level(patch));
}

bool MultiresSurface::patch_resident(std::uint32_t level, std::uint32_t patch) const {
    if (!state_ || !state_->level_ok(level)) return false;
    return state_->levels[level].keeps(patch);
}

bool MultiresSurface::uniform_depth() const {
    if (!state_) return true;
    for (const MultiresLevel& l : state_->levels)
        if (!l.patch_kept.empty()) return false;
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
    // Every layer's field at the level that just went is discarded with it: a
    // coefficient stored against a level that no longer exists names nothing.
    // Destructive, and the owner above this is what makes it reversible — the
    // same statement this call already makes about the base detail it hands
    // back.
    sync_stack_levels(*state_);
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
    // A SCULPT LAYER IS DETAIL, under a different owner. The stencils above the
    // cage are defined by the connectivity being replaced, so a layer's
    // coefficients would become as meaningless as the base's — and refusing for
    // one while accepting the other would be a rule nobody could predict.
    for (std::size_t i = 0; i < state_->stack.size(); ++i)
        if (state_->stack.at(i)->has_content()) {
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
    for (std::uint32_t l = 1; l < levels; ++l) {
        // A REGIONAL LEVEL IS REBUILT OVER THE SAME PATCH IDS. They are level-0
        // face ids, and the new cage's faces are its own — so this succeeds
        // where the new cage keeps the numbering and is REFUSED, by the same
        // residency check every regional level goes through, where it does not.
        // Rebuilding a regional hierarchy densely would be the quiet answer,
        // and it would silently multiply what the artist chose to pay for.
        const std::vector<char>& keep = state_->levels[l].patch_kept;
        bool ok = false;
        if (keep.empty()) {
            ok = rebuilt->add_level(&err);
        } else {
            std::vector<std::uint32_t> patches;
            for (std::uint32_t p = 0; p < keep.size(); ++p)
                if (keep[p]) patches.push_back(p);
            ok = rebuilt->add_level_for_patches(patches, &err);
        }
        if (!ok) {
            if (out_error) *out_error = err;
            return false;
        }
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
    // The stack's composition and content bumps are FOLDED IN rather than kept
    // apart, so a host written against this ABI before sculpt layers existed
    // sees a strength change exactly as it sees a coefficient write. A host
    // that needs to know which of the three kinds of layer change happened
    // reads the three counters the stack keeps.
    return state_ ? state_->detail_revision + state_->stack.geometry_bumps() : 0;
}
std::uint64_t MultiresSurface::evaluated_revision() const {
    return state_ ? state_->evaluated_revision + state_->stack.geometry_bumps() : 0;
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
    LevelCache* cache = s.levels[level].cache.get();
    ChunkTable* level_chunks =
        cache != nullptr && !cache->face_chunk.empty() ? &cache->chunks : nullptr;
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
            // The SAME event in the shared vocabulary, and only when the level
            // has been partitioned — nothing here builds a chunk table on a
            // sculpting path. The base patch stays the hierarchy's own dirty
            // unit because a patch is what dirty propagation is defined over;
            // the chunk is what a host uploads. They are the same granularity
            // keyed two ways, which is why one walk marks both rather than two
            // walks disagreeing.
            if (level_chunks != nullptr && faces[i] < s.levels[level].cache->face_chunk.size())
                level_chunks->mark(s.levels[level].cache->face_chunk[faces[i]],
                                   ChunkDirty::Geometry);
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
        // `patch_kept` is topology: it is the authoritative record of WHICH
        // faces this level has, and everything else regional is derived from
        // it.
        m.topology += l.topology.bytes() + l.patch_kept.capacity();
        m.detail += l.detail.bytes();
        if (l.composed) m.composed += l.composed->bytes();
        if (!l.cache) continue;
        ++m.resident_levels;
        const LevelCache::Bytes split = l.cache->byte_split();
        m.evaluated += split.evaluated;
        m.runtime_index += split.runtime;
        m.chunk_index += split.chunk_index;
    }
    for (const AttrLevel& a : state_->attr) m.runtime_index += a.bytes();
    // MEMORY MULTIPLIES TWICE with a layer stack, and the report says so rather
    // than hiding it: a layer costs its coverage per level, a hundred layers
    // over one cheek cost a hundred copies of that coverage, and the composed
    // field costs the union of them once more. Nothing here caps anything — a
    // cap that silently stopped recording would leave the pass on the surface
    // and un-dialable, which is a correctness bug wearing a memory limit's
    // clothes. The levers are merge, bake and delete, and a host reaches for
    // them by reading this.
    const SculptLayerMemory layers = state_->stack.memory();
    m.sculpt_layers = layers.content + layers.masks;
    m.authoritative = m.base + m.topology + m.detail + m.sculpt_layers;
    m.rebuildable = m.evaluated + m.composed + m.runtime_index + m.chunk_index;
    m.total = m.authoritative + m.rebuildable;
    return m;
}

bool MultiresSurface::level_resident(std::uint32_t level) const {
    return state_ && state_->level_ok(level) && state_->levels[level].cache != nullptr;
}

namespace {

// RELEASING storage moves the generation for the same reason CREATING it does.
//
// `cache_generation` is what a `MeshSculptor` bound to a level's `Mesh`
// compares to decide whether the reference it is holding still points into live
// storage, and `ensure_cache` bumped it on the way in. Nothing bumped it on the
// way out, which left a window one call wide — drop, then stamp — where the
// number had not moved and `MultiresSculptor::bind` therefore kept a sculptor
// whose `Mesh&` pointed into a freed `LevelCache`.
//
// WHAT THAT LOOKED LIKE was not a crash, which is why it survived: the stamp
// wrote its displacement into released storage, `absorb_level_edit` rebuilt the
// level from the authoritative detail before reading it back, and the dab
// simply was not there — with the sculptor still returning the number of weld
// classes it believed it had moved. Under a memory warning arriving mid-stroke
// that is a brush that stops working for one dab and says nothing.
void release_generation(MultiresSurface::State& s, bool released) {
    if (released) ++s.cache_generation;
}

}  // namespace

void MultiresSurface::drop_all_caches() {
    if (!state_) return;
    bool released = false;
    for (MultiresLevel& l : state_->levels) {
        released = released || l.cache != nullptr;
        l.cache.reset();
        // The composed detail goes with the rest: it is `B + Σ s·m·L` and
        // every input to it is still here, so it rebuilds bit-identically.
        l.composed.reset();
        l.pending.clear();
        l.pending_all = true;
    }
    state_->base_rest.reset();
    state_->attr.clear();
    state_->base_frames_all = true;
    state_->base_frames_dirty.clear();
    release_generation(*state_, released);
}

void MultiresSurface::drop_intermediate_caches() {
    if (!state_) return;
    const std::uint32_t keep_a = std::min(state_->sculpt_level, max_level());
    const std::uint32_t keep_b = std::min(state_->display_level, max_level());
    // Flush first: a level holding unconsumed pending work is a level whose
    // changes have not reached the one above it yet, and dropping it would drop
    // the edit rather than the cache.
    evaluate_up_to(*state_, std::max(keep_a, keep_b));
    bool released = false;
    for (std::uint32_t l = 0; l < state_->levels.size(); ++l) {
        if (l == keep_a || l == keep_b) continue;
        released = released || state_->levels[l].cache != nullptr;
        state_->levels[l].cache.reset();
        state_->levels[l].composed.reset();
    }
    state_->attr.clear();
    release_generation(*state_, released);
}

void MultiresSurface::drop_inactive_caches() {
    if (!state_) return;
    // Evaluating a level reads its parent's positions, so the levels BELOW the
    // ones in use have to stay. What goes is everything above them, which on a
    // deep hierarchy is most of it.
    const std::uint32_t keep = std::max(state_->sculpt_level, state_->display_level);
    bool released = false;
    for (std::uint32_t l = keep + 1; l < state_->levels.size(); ++l) {
        released = released || state_->levels[l].cache != nullptr;
        state_->levels[l].cache.reset();
        state_->levels[l].composed.reset();
        state_->levels[l].pending.clear();
        state_->levels[l].pending_all = true;
    }
    state_->attr.resize(std::min<std::size_t>(state_->attr.size(), keep + 1u));
    release_generation(*state_, released);
}

}  // namespace mesh
}  // namespace clay
