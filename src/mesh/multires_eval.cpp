#include <algorithm>

#include "multires_internal.h"

namespace clay {
namespace mesh {
namespace {

// The cage's UVs, per geometric vertex — the tangent source for the base
// frames, and only when the cage has no attribute splits. A seam's two sides
// carry different UVs at one geometric point, so a UV tangent there is
// discontinuous ACROSS the seam and the detail either side of it would sit at
// different angles; the geometric tangent has no such problem and costs only an
// arbitrary direction.
bool class_uvs(const MultiresSurface::State& s, std::vector<kernel::cfloat2>* out) {
    if (s.attribute_split || s.base.uvs.size() != s.base.positions.size()) return false;
    out->assign(s.class_count, kernel::cf2(0, 0));
    for (std::size_t v = 0; v < s.base.uvs.size(); ++v) (*out)[s.class_of[v]] = s.base.uvs[v];
    return true;
}

LevelCache& ensure_cache(MultiresSurface::State& s, std::uint32_t level) {
    MultiresLevel& lev = s.levels[level];
    if (!lev.cache) {
        lev.cache = std::make_unique<LevelCache>();
        lev.cache->conn = LevelConnectivity::build(lev.topology);
        lev.cache->evaluated = false;
        ++s.cache_generation;
    }
    return *lev.cache;
}

// P(n) = S(n) + Frame(n) * Detail(n), for these vertices. The one place the
// model in `multires.h` is actually written down in code.
void apply_detail(MultiresLevel& lev, const std::vector<std::uint32_t>& vertices) {
    LevelCache& c = *lev.cache;
    for (std::uint32_t v : vertices) {
        const LocalDetail d = lev.detail.get(v);
        c.mesh.positions[v] =
            c.subdivided[v] + frame_to_world(c.frames[v], d.tangent, d.bitangent, d.normal);
    }
}

void apply_detail_all(MultiresLevel& lev) {
    LevelCache& c = *lev.cache;
    const std::uint32_t n = lev.topology.vertex_count;
    c.mesh.positions.resize(n);
    for (std::uint32_t v = 0; v < n; ++v) {
        const LocalDetail d = lev.detail.get(v);
        c.mesh.positions[v] =
            c.subdivided[v] + frame_to_world(c.frames[v], d.tangent, d.bitangent, d.normal);
    }
}

// The cage's own normals and frames, refreshed where it moved. Level 0 is the
// one level where S and P are the same array, so a sculpt there moves the
// surface the frames are built from — which is exactly why the detail above it
// follows the form.
void refresh_base_frames(MultiresSurface::State& s) {
    MultiresLevel& lev = s.levels[0];
    LevelCache& c = *lev.cache;
    std::vector<kernel::cfloat2> uvs;
    const bool have_uvs = class_uvs(s, &uvs);
    const std::vector<kernel::cfloat2>* uv_ptr = have_uvs ? &uvs : nullptr;

    if (s.base_frames_all) {
        level_normals(lev.topology, c.conn, c.mesh.positions, &c.mesh.normals);
        build_base_frames(lev.topology, c.conn, c.mesh.positions, c.mesh.normals, uv_ptr,
                          &c.frames);
    } else if (!s.base_frames_dirty.empty()) {
        expand_by_face_ring(lev.topology, c.conn, s.base_frames_dirty, &s.scratch_mark,
                            &s.scratch_c);
        level_normals_partial(lev.topology, c.conn, c.mesh.positions, s.scratch_c, &c.mesh.normals);
        build_base_frames_partial(lev.topology, c.conn, c.mesh.positions, c.mesh.normals, uv_ptr,
                                  s.scratch_c, &c.frames);
    }
    s.base_frames_all = false;
    s.base_frames_dirty.clear();
}

// Display normals a direct detail write left stale — an undo replaying a
// gesture, or a verb writing coefficients rather than positions. Costs the
// footprint and nothing when there is none.
void drain_normals_pending(MultiresSurface::State& s, std::uint32_t level) {
    MultiresLevel& lev = s.levels[level];
    if (lev.normals_pending.empty() || !lev.cache) return;
    LevelCache& c = *lev.cache;
    expand_by_face_ring(lev.topology, c.conn, lev.normals_pending, &s.scratch_mark, &s.scratch_c);
    level_normals_partial(lev.topology, c.conn, c.mesh.positions, s.scratch_c, &c.mesh.normals);
    lev.normals_pending.clear();
}

void evaluate_level0(MultiresSurface::State& s) {
    MultiresLevel& lev = s.levels[0];
    LevelCache& c = ensure_cache(s, 0);
    if (!c.evaluated) {
        gather_class_positions(s, &c.mesh.positions);
        c.subdivided.clear();  // S(0) IS P(0); see LevelCache
        s.base_frames_all = true;
        c.evaluated = true;
        lev.pending_all = true;
    }
    refresh_base_frames(s);
    drain_normals_pending(s, 0);
}

void full_evaluate(MultiresSurface::State& s, std::uint32_t level) {
    const MultiresLevel& parent = s.levels[level - 1];
    MultiresLevel& lev = s.levels[level];
    const LevelCache& pc = *parent.cache;
    LevelCache& c = *lev.cache;

    subdivide_positions(parent.topology, pc.conn, pc.mesh.positions, &c.subdivided);
    // The S-normals exist only to build the frames, which then carry them —
    // so they go into scratch rather than into a per-level array nobody would
    // read again.
    if (s.scratch_normals.size() < lev.topology.vertex_count)
        s.scratch_normals.resize(lev.topology.vertex_count, kernel::cf3(0, 1, 0));
    level_normals(lev.topology, c.conn, c.subdivided, &s.scratch_normals);
    transport_frames(parent.topology, pc.conn, pc.frames, s.scratch_normals, &c.frames);
    apply_detail_all(lev);
    level_normals(lev.topology, c.conn, c.mesh.positions, &c.mesh.normals);
    s.stats.vertices_evaluated += lev.topology.vertex_count;
    s.stats.normals_recomputed += lev.topology.vertex_count;
    ++s.stats.full_level_rebuilds;
    c.evaluated = true;
    lev.normals_pending.clear();
}

void partial_evaluate(MultiresSurface::State& s, std::uint32_t level) {
    const MultiresLevel& parent = s.levels[level - 1];
    MultiresLevel& lev = s.levels[level];
    const LevelCache& pc = *parent.cache;
    LevelCache& c = *lev.cache;

    // The children whose SUBDIVIDED position the parent's motion reaches...
    dirty_children(parent.topology, pc.conn, parent.pending, &s.scratch_a);
    // ...and the halo around them, whose normals — and therefore frames, and
    // therefore reconstructed detail — moved even though their subdivided
    // position did not.
    expand_by_face_ring(lev.topology, c.conn, s.scratch_a, &s.scratch_mark, &s.scratch_b);

    subdivide_positions_partial(parent.topology, pc.conn, pc.mesh.positions, s.scratch_a,
                                &c.subdivided);
    if (s.scratch_normals.size() < lev.topology.vertex_count)
        s.scratch_normals.resize(lev.topology.vertex_count, kernel::cf3(0, 1, 0));
    level_normals_partial(lev.topology, c.conn, c.subdivided, s.scratch_b, &s.scratch_normals);
    transport_frames_partial(parent.topology, pc.conn, pc.frames, s.scratch_normals, s.scratch_b,
                             &c.frames);
    apply_detail(lev, s.scratch_b);

    // The display normals reach one ring further again, because a vertex that
    // did not move still shades differently when its neighbour did.
    expand_by_face_ring(lev.topology, c.conn, s.scratch_b, &s.scratch_mark, &s.scratch_c);
    level_normals_partial(lev.topology, c.conn, c.mesh.positions, s.scratch_c, &c.mesh.normals);

    s.stats.vertices_evaluated += s.scratch_b.size();
    s.stats.normals_recomputed += s.scratch_c.size();
    ++s.stats.partial_level_updates;

    lev.pending.insert(lev.pending.end(), s.scratch_b.begin(), s.scratch_b.end());
    mark_patches(s, level, s.scratch_b);
}

}  // namespace

void expand_by_face_ring(const LevelTopology& topology, const LevelConnectivity& conn,
                         const std::vector<std::uint32_t>& in, std::vector<char>* mark,
                         std::vector<std::uint32_t>* out) {
    if (mark->size() < topology.vertex_count) mark->assign(topology.vertex_count, 0);
    out->clear();
    const auto push = [&](std::uint32_t v) {
        if (v < topology.vertex_count && !(*mark)[v]) {
            (*mark)[v] = 1;
            out->push_back(v);
        }
    };
    for (std::uint32_t v : in) {
        if (v >= topology.vertex_count) continue;
        push(v);
        std::size_t count = 0;
        const std::uint32_t* faces = conn.faces_of(v, &count);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t arity = 0;
            const std::uint32_t* corners = topology.face(faces[i], &arity);
            for (std::uint32_t k = 0; k < arity; ++k) push(corners[k]);
        }
    }
    // Reset through the LIST rather than clearing the array, so the cost is the
    // footprint and not the level — the discipline `WalkScratch` already uses.
    for (std::uint32_t v : *out) (*mark)[v] = 0;
    std::sort(out->begin(), out->end());
}

const LevelConnectivity& connectivity_of(MultiresSurface::State& s, std::uint32_t level) {
    return ensure_cache(s, level).conn;
}

void evaluate_up_to(MultiresSurface::State& s, std::uint32_t level) {
    if (s.levels.empty()) return;
    const std::uint32_t target = std::min(level, static_cast<std::uint32_t>(s.levels.size() - 1));
    evaluate_level0(s);
    for (std::uint32_t l = 1; l <= target; ++l) {
        ensure_cache(s, l);
        // A level with no cache, a level that was never evaluated, and a level
        // whose parent changed everywhere are the same case: rebuild it whole.
        if (!s.levels[l].cache->evaluated || s.levels[l - 1].pending_all) {
            full_evaluate(s, l);
            s.levels[l].pending_all = true;
            s.levels[l].pending.clear();
        } else {
            if (!s.levels[l - 1].pending.empty()) partial_evaluate(s, l);
            drain_normals_pending(s, l);
        }
        s.levels[l - 1].pending.clear();
        s.levels[l - 1].pending_all = false;
    }
}

// -- attributes ---------------------------------------------------------------

bool ensure_attributes(MultiresSurface::State& s, std::uint32_t level) {
    const bool has_uvs = s.base.uvs.size() == s.base.positions.size() && !s.base.uvs.empty();
    const bool has_colors = s.base.colors.size() == s.base.positions.size() && !s.base.colors.empty();
    if (!has_uvs && !has_colors && !s.attribute_split) return false;
    if (s.attr.size() > level) return true;

    if (s.attr.empty()) {
        AttrLevel a0;
        if (s.attribute_split) {
            // The cage's RAW connectivity: a seam's two duplicate vertices stay
            // two vertices here, so the boundary rule interpolates each side of
            // the seam along itself. That is the whole of "SHALL NOT average a
            // UV across a seam" — it is a construction rather than a check.
            std::vector<std::uint32_t> identity(s.base.positions.size());
            for (std::size_t v = 0; v < identity.size(); ++v)
                identity[v] = static_cast<std::uint32_t>(v);
            if (!base_topology_from_mesh(s.base, identity.data(),
                                         static_cast<std::uint32_t>(identity.size()),
                                         &a0.topology))
                return false;
            a0.conn = LevelConnectivity::build(a0.topology);
            a0.to_geom = s.class_of;
        }
        if (has_uvs) a0.uvs = s.base.uvs;
        if (has_colors) a0.colors = s.base.colors;
        s.attr.push_back(std::move(a0));
    }

    for (std::uint32_t l = static_cast<std::uint32_t>(s.attr.size());
         l <= level && l < s.levels.size(); ++l) {
        // Built into a local and appended, so a reference into `s.attr` cannot
        // outlive a reallocation of the vector holding it.
        AttrLevel a;
        const bool split = s.attribute_split;
        const LevelTopology& ptopo = split ? s.attr[l - 1].topology : s.levels[l - 1].topology;
        const LevelConnectivity& pconn = split ? s.attr[l - 1].conn : connectivity_of(s, l - 1);
        if (split) {
            a.topology = subdivide_topology(ptopo, pconn);
            a.conn = LevelConnectivity::build(a.topology);
            // The two hierarchies emit child faces in the same order — parent
            // face, then corner — so face f of one is face f of the other and
            // the map falls out of walking the corners side by side.
            const LevelTopology& gt = s.levels[l].topology;
            a.to_geom.assign(a.topology.vertex_count, 0u);
            const std::size_t corners = std::min(a.topology.corners.size(), gt.corners.size());
            for (std::size_t i = 0; i < corners; ++i) a.to_geom[a.topology.corners[i]] = gt.corners[i];
        }
        if (!s.attr[l - 1].uvs.empty())
            subdivide_attribute(ptopo, pconn, s.attr[l - 1].uvs, &a.uvs);
        if (!s.attr[l - 1].colors.empty())
            subdivide_attribute(ptopo, pconn, s.attr[l - 1].colors, &a.colors);
        s.attr.push_back(std::move(a));
    }
    return s.attr.size() > level;
}

// -- the public evaluation surface --------------------------------------------

const LevelTopology& MultiresSurface::topology_at(std::uint32_t level) const {
    static const LevelTopology kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    return state_->levels[level].topology;
}

const LevelConnectivity& MultiresSurface::connectivity_at(std::uint32_t level) {
    static const LevelConnectivity kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    return connectivity_of(*state_, level);
}

const std::vector<kernel::cfloat3>& MultiresSurface::positions_at(std::uint32_t level) {
    static const std::vector<kernel::cfloat3> kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    evaluate_up_to(*state_, level);
    return state_->levels[level].cache->mesh.positions;
}

const std::vector<kernel::cfloat3>& MultiresSurface::subdivided_at(std::uint32_t level) {
    static const std::vector<kernel::cfloat3> kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    evaluate_up_to(*state_, level);
    // Level 0 has no parent to subdivide, so S and P are one array.
    return level == 0 ? state_->levels[0].cache->mesh.positions
                      : state_->levels[level].cache->subdivided;
}

const std::vector<SurfaceFrame>& MultiresSurface::frames_at(std::uint32_t level) {
    static const std::vector<SurfaceFrame> kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    evaluate_up_to(*state_, level);
    return state_->levels[level].cache->frames;
}

const std::vector<kernel::cfloat3>& MultiresSurface::normals_at(std::uint32_t level) {
    static const std::vector<kernel::cfloat3> kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    evaluate_up_to(*state_, level);
    return state_->levels[level].cache->mesh.normals;
}

Mesh& MultiresSurface::level_mesh(std::uint32_t level) {
    static Mesh scratch;
    if (!state_ || !state_->level_ok(level)) return scratch;
    evaluate_up_to(*state_, level);
    LevelCache& c = *state_->levels[level].cache;
    if (!c.faces_built) {
        level_faces_into(state_->levels[level].topology, &c.mesh);
        c.faces_built = true;
    }
    return c.mesh;
}

const Adjacency& MultiresSurface::level_adjacency(std::uint32_t level) {
    static const Adjacency kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    Mesh& m = level_mesh(level);
    LevelCache& c = *state_->levels[level].cache;
    if (!c.adjacency) {
        // EXACT welding: a level's vertices are already the geometric points of
        // the surface, so an epsilon here would fuse a thin wall to itself for
        // no benefit. Two vertices that genuinely coincide bit for bit still
        // weld, which is what keeps a degenerate cage from cracking.
        c.adjacency = std::make_unique<Adjacency>(Adjacency::build(m, 0.0f));
    }
    return *c.adjacency;
}

bool MultiresSurface::build_block(std::uint32_t level, std::uint32_t patch, Block* out) {
    if (!out || !state_ || !state_->level_ok(level)) return false;
    const LevelTopology& t = state_->levels[level].topology;
    if (patch >= t.patch_count && !(t.face_patch.empty() && patch < t.face_count)) return false;
    evaluate_up_to(*state_, level);

    out->patch = patch;
    out->level = level;
    out->vertices.clear();
    out->indices.clear();
    // Two passes over the patch's faces rather than a hash map: the first
    // collects the vertices and sorts them, the second rewrites the corners
    // through a binary search. The order is then a function of the topology
    // alone, so two hosts asking for the same block get the same buffer.
    for (std::uint32_t f = 0; f < t.face_count; ++f) {
        if (t.patch_of(f) != patch) continue;
        std::uint32_t arity = 0;
        const std::uint32_t* c = t.face(f, &arity);
        for (std::uint32_t k = 0; k < arity; ++k) out->vertices.push_back(c[k]);
    }
    std::sort(out->vertices.begin(), out->vertices.end());
    out->vertices.erase(std::unique(out->vertices.begin(), out->vertices.end()),
                        out->vertices.end());
    const auto local_of = [&](std::uint32_t v) {
        return static_cast<std::uint32_t>(
            std::lower_bound(out->vertices.begin(), out->vertices.end(), v) -
            out->vertices.begin());
    };
    for (std::uint32_t f = 0; f < t.face_count; ++f) {
        if (t.patch_of(f) != patch) continue;
        std::uint32_t arity = 0;
        const std::uint32_t* c = t.face(f, &arity);
        for (std::uint32_t i = 2; i < arity; ++i)
            out->indices.insert(out->indices.end(),
                                {local_of(c[0]), local_of(c[i - 1]), local_of(c[i])});
    }
    return true;
}

namespace {

// What a level's export should carry: each attribute the CAGE carried and the
// caller still wants. A hierarchy over a mesh with no colours exports none, so
// a layer's attribute set does not change under a round trip.
struct ExportWants {
    bool normals = false;
    bool uvs = false;
    bool colors = false;
};

ExportWants wants_of(const MultiresSurface::State& s, const MultiresExportOptions& options) {
    const std::size_t n = s.base.positions.size();
    ExportWants w;
    w.normals = options.normals && !s.base.normals.empty();
    w.uvs = options.uvs && s.base.uvs.size() == n && !s.base.uvs.empty();
    w.colors = options.colors && s.base.colors.size() == n && !s.base.colors.empty();
    return w;
}

// The cage's attribute connectivity IS its geometric one, so the export is the
// level with the subdivided attributes laid over it vertex for vertex.
void export_direct(MultiresSurface::State& s, std::uint32_t level, const ExportWants& wants,
                   bool have_attrs, Mesh* out) {
    const LevelCache& c = *s.levels[level].cache;
    level_faces_into(s.levels[level].topology, out);
    out->positions = c.mesh.positions;
    if (wants.normals) out->normals = c.mesh.normals;
    if (!have_attrs) return;
    if (wants.uvs) out->uvs = s.attr[level].uvs;
    if (wants.colors) out->colors = s.attr[level].colors;
}

// The cage splits a geometric point into several export vertices — how a flat
// mesh writes a seam — so the export runs over the ATTRIBUTE topology and reads
// geometry through the map.
void export_split(MultiresSurface::State& s, std::uint32_t level, const ExportWants& wants,
                  Mesh* out) {
    const LevelCache& c = *s.levels[level].cache;
    const AttrLevel& a = s.attr[level];
    level_faces_into(a.topology, out);
    out->positions.resize(a.topology.vertex_count);
    for (std::uint32_t v = 0; v < a.topology.vertex_count; ++v)
        out->positions[v] = c.mesh.positions[a.to_geom[v]];
    if (wants.normals && c.mesh.normals.size() == c.mesh.positions.size()) {
        out->normals.resize(a.topology.vertex_count);
        for (std::uint32_t v = 0; v < a.topology.vertex_count; ++v)
            out->normals[v] = c.mesh.normals[a.to_geom[v]];
    }
    if (wants.uvs && a.uvs.size() == a.topology.vertex_count) out->uvs = a.uvs;
    if (wants.colors && a.colors.size() == a.topology.vertex_count) out->colors = a.colors;
}

}  // namespace

Mesh MultiresSurface::mesh_at_level(std::uint32_t level, const MultiresExportOptions& options,
                                    const parallel::CancelToken* cancel) {
    Mesh out;
    if (!state_ || !state_->level_ok(level)) return out;
    evaluate_up_to(*state_, level);
    // A cancelled export returns an EMPTY mesh rather than a partial one: a
    // caller that ignored the cancel and drew the result would draw a fraction
    // of the model, which is worse than drawing nothing.
    if (cancel && cancel->cancelled()) return out;

    const ExportWants wants = wants_of(*state_, options);
    const bool need_attrs = wants.uvs || wants.colors || state_->attribute_split;
    const bool have_attrs = need_attrs ? ensure_attributes(*state_, level) : false;

    if (!state_->attribute_split || !have_attrs)
        export_direct(*state_, level, wants, have_attrs, &out);
    else
        export_split(*state_, level, wants, &out);
    return out;
}

// -- editing ------------------------------------------------------------------

void MultiresSurface::absorb_level_edit(std::uint32_t level,
                                        const std::vector<std::uint32_t>& vertices) {
    if (!state_ || !state_->level_ok(level) || vertices.empty()) return;
    evaluate_up_to(*state_, level);
    MultiresLevel& lev = state_->levels[level];
    LevelCache& c = *lev.cache;

    if (level == 0) {
        // The cage is authoritative: what the brush wrote IS the geometry, and
        // it is copied to every raw vertex of the class so a seam's duplicates
        // stay coincident and cannot open into a crack.
        for (std::uint32_t v : vertices) {
            if (v >= state_->class_count) continue;
            const kernel::cfloat3 p = c.mesh.positions[v];
            for (std::uint32_t i = state_->class_offsets[v]; i < state_->class_offsets[v + 1]; ++i)
                state_->base.positions[state_->class_members[i]] = p;
        }
        state_->base_frames_dirty.insert(state_->base_frames_dirty.end(), vertices.begin(),
                                         vertices.end());
        ++state_->base_revision;
    } else {
        // Above the cage the surface is the subdivision plus a residual, so
        // what is stored is the residual: the difference between where the
        // brush left the vertex and where the subdivision would have put it,
        // read in the transported frame.
        for (std::uint32_t v : vertices) {
            if (v >= lev.topology.vertex_count) continue;
            const kernel::cfloat3 delta = c.mesh.positions[v] - c.subdivided[v];
            LocalDetail d;
            world_to_frame(c.frames[v], delta, &d.tangent, &d.bitangent, &d.normal);
            lev.detail.set(v, d);
            // AND READ IT BACK. The stored coefficients are authoritative, so
            // the cached position has to be what they reconstruct to — not what
            // the brush wrote, which differs from it by the last bits of a
            // round trip through the frame. Without this the surface a host is
            // looking at and the surface a reload would produce are a few ulps
            // apart, and a redo lands on neither.
            c.mesh.positions[v] =
                c.subdivided[v] + frame_to_world(c.frames[v], d.tangent, d.bitangent, d.normal);
        }
        ++state_->detail_revision;
    }
    lev.pending.insert(lev.pending.end(), vertices.begin(), vertices.end());
    mark_patches(*state_, level, vertices);
    ++state_->evaluated_revision;
}

void MultiresSurface::set_detail(std::uint32_t level, std::uint32_t vertex,
                                 const LocalDetail& value) {
    if (!state_ || !state_->level_ok(level) || level == 0) return;
    MultiresLevel& lev = state_->levels[level];
    if (vertex >= lev.topology.vertex_count) return;
    lev.detail.set(vertex, value);
    if (lev.cache && lev.cache->evaluated) {
        LevelCache& c = *lev.cache;
        c.mesh.positions[vertex] =
            c.subdivided[vertex] +
            frame_to_world(c.frames[vertex], value.tangent, value.bitangent, value.normal);
        lev.normals_pending.push_back(vertex);
    }
    lev.pending.push_back(vertex);
    mark_patches(*state_, level, {vertex});
    ++state_->detail_revision;
    ++state_->evaluated_revision;
}

void MultiresSurface::set_base_position(std::uint32_t vertex, kernel::cfloat3 position) {
    if (!state_ || state_->levels.empty() || vertex >= state_->class_count) return;
    for (std::uint32_t i = state_->class_offsets[vertex]; i < state_->class_offsets[vertex + 1];
         ++i)
        state_->base.positions[state_->class_members[i]] = position;
    MultiresLevel& lev = state_->levels[0];
    if (lev.cache && lev.cache->evaluated) lev.cache->mesh.positions[vertex] = position;
    state_->base_frames_dirty.push_back(vertex);
    lev.pending.push_back(vertex);
    lev.normals_pending.push_back(vertex);
    mark_patches(*state_, 0, {vertex});
    ++state_->base_revision;
    ++state_->evaluated_revision;
}

}  // namespace mesh
}  // namespace clay
