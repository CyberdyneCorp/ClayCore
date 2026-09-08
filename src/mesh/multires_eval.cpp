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

// Does this level hold a cache with a SURFACE in it?
//
// Not the same question as "does it hold a cache". `ensure_cache` allocates one
// and builds the level's connectivity for a caller that wanted only that —
// `connectivity_of`, which `MultiresSurface::connectivity_at` exposes — and
// leaves `subdivided`, `frames` and `mesh.positions` empty behind an
// `evaluated` flag that is still false. Every reader of a level's POSITIONS has
// to ask this one, because a released level whose connectivity was asked for
// since is indistinguishable from an evaluated one by the pointer alone.
bool level_is_evaluated(const MultiresSurface::State& s, std::uint32_t level) {
    return s.levels[level].cache && s.levels[level].cache->evaluated;
}

// P(n) = S(n) + Frame(n) * Detail(n), for these vertices. The one place the
// model in `multires.h` is actually written down in code.
//
// `Detail(n)` IS `effective_detail`, which is the level's composed field when a
// sculpt layer reaches this level and the level's own base field when none
// does. The widening is deliberately confined to that one call: with no stack
// the read is the read it always was, so a hierarchy with no layers evaluates
// to the same bits it did before layers existed. An implementation that always
// composed — even summing an empty stack — would move the last bits of every
// vertex and break every existing golden for nothing.
void apply_detail(MultiresLevel& lev, const std::vector<std::uint32_t>& vertices) {
    LevelCache& c = *lev.cache;
    const DetailField& field = effective_detail(lev);
    for (std::uint32_t v : vertices) {
        const LocalDetail d = field.get(v);
        c.mesh.positions[v] =
            c.subdivided[v] + frame_to_world(c.frames[v], d.tangent, d.bitangent, d.normal);
    }
}

void apply_detail_all(MultiresLevel& lev) {
    LevelCache& c = *lev.cache;
    const DetailField& field = effective_detail(lev);
    const std::uint32_t n = lev.topology.vertex_count;
    c.mesh.positions.resize(n);
    for (std::uint32_t v = 0; v < n; ++v) {
        const LocalDetail d = field.get(v);
        c.mesh.positions[v] =
            c.subdivided[v] + frame_to_world(c.frames[v], d.tangent, d.bitangent, d.normal);
    }
}

// P(0) for a cage carrying BASE DEFORMATION LAYERS: the cage's own position
// plus the stack's contribution, read in the cage's REST frame. Written from
// the rest positions rather than accumulated onto the cached ones, so it is
// idempotent — a re-evaluation after a strength change must land on the same
// answer as a re-evaluation from cold.
//
// Nothing at all when no layer reaches level 0, which is every hierarchy that
// has no proportion pass on it.
void apply_base_layers(MultiresSurface::State& s, const std::vector<std::uint32_t>* vertices) {
    if (s.levels.empty() || !s.levels[0].composed) return;
    const MultiresSurface::State::BaseRestFrames* rest = base_rest_frames(s);
    if (!rest) return;
    LevelCache& c = *s.levels[0].cache;
    const DetailField& field = *s.levels[0].composed;
    const auto write = [&](std::uint32_t v) {
        if (v >= rest->positions.size() || v >= c.mesh.positions.size()) return;
        const LocalDetail d = field.get(v);
        c.mesh.positions[v] =
            rest->positions[v] + frame_to_world(rest->frames[v], d.tangent, d.bitangent, d.normal);
    };
    if (vertices) {
        for (std::uint32_t v : *vertices) write(v);
        return;
    }
    for (std::uint32_t v = 0; v < static_cast<std::uint32_t>(c.mesh.positions.size()); ++v)
        write(v);
}

// The cage's own normals and frames, refreshed where it moved. Level 0 is the
// one level where S and P are the same array, so a sculpt there moves the
// surface the frames are built from — which is exactly why the detail above it
// follows the form.
// The neighbourhood for a level whose parent is ALREADY EVALUATED.
//
// `MultiresSurface::cross_level_at` is the outside door and begins with
// `evaluate_up_to`; calling it from inside the evaluation would recurse into the
// evaluation that is running. This is the same body with that step removed.
//
// EXACTLY FOUR CALLERS, NAMED because the precondition is silent when it breaks
// and a described property is easier to talk past than a list:
//
//   full_evaluate, partial_evaluate, reapply_recomposed, drain_normals_pending
//
// All four are reached only from `evaluate_up_to`, and it is that function's
// LOOP ORDER that holds the precondition rather than anything the callers do:
// it walks levels 1..target in order, so level `l - 1` was evaluated on the
// previous iteration, or is level 0 and was evaluated by `evaluate_level0`
// before the loop. The one call outside the loop — `drain_normals_pending` on
// the `below_is_current` early return — holds it for the other reason: nothing
// below moved, so the parent is still current from a previous pass.
//
// A FIFTH CALLER MUST ARGUE WITH THIS COMMENT. The guard below returns null
// when the parent is not evaluated, which is safe — nothing reads a released
// cache — but it is NOT a fix: a null neighbourhood means the level's own ring
// only, which is precisely the incomplete half-ring this whole path exists to
// complete. So a caller that does not hold the precondition gets the OLD WRONG
// NORMALS BACK, silently, and no test of the evaluation will fail. That is the
// failure mode to weigh before adding one.
//
// Null rather than an empty object when there is no depth boundary, so the
// readers pay one null check and no walk.
const CrossLevelNeighborhood* cross_level_of(MultiresSurface::State& s, std::uint32_t level) {
    if (level == 0 || !s.level_ok(level)) return nullptr;
    LevelCache* c = s.levels[level].cache.get();
    if (c == nullptr) return nullptr;
    // A LEVEL THAT NEVER HAD ONE, decided from the topology alone. This is the
    // only legitimate way for a regional path to see no neighbourhood, and
    // separating it from "released" by name is what lets the release exist at
    // all -- see `LevelCache::cross_released`.
    if (level_is_self_contained(s.levels[level].topology, s.levels[level].patch_kept))
        return nullptr;
    if (!level_is_evaluated(s, level - 1)) return nullptr;
    const MultiresLevel& parent = s.levels[level - 1];
    // A RELEASED NEIGHBOURHOOD REBUILDS, it does not read as absent — and it
    // needs no mark to do so, because `level_is_self_contained` above decides
    // "never had one" from the TOPOLOGY. A null pointer here therefore means
    // exactly one thing: this level needs a neighbourhood and does not have it.
    ++s.stats.cross_level_reads;
    if (!c->cross) {
        c->cross = std::make_unique<CrossLevelNeighborhood>(
            build_cross_level(parent.topology, parent.cache->conn, parent.cache->mesh.positions,
                              s.levels[level].topology, s.levels[level].patch_kept));
        c->cross_parent_revision = parent.positions_revision();
    } else if (c->cross_parent_revision != parent.positions_revision()) {
        ++s.stats.cross_level_refreshes;
        refresh_cross_level(parent.topology, parent.cache->conn, parent.cache->mesh.positions,
                            c->cross.get());
        c->cross_parent_revision = parent.positions_revision();
    }
    return c->cross->empty() ? nullptr : c->cross.get();
}

void refresh_base_frames(MultiresSurface::State& s) {
    MultiresLevel& lev = s.levels[0];
    LevelCache& c = *lev.cache;
    std::vector<kernel::cfloat2> uvs;
    const bool have_uvs = class_uvs(s, &uvs);
    const std::vector<kernel::cfloat2>* uv_ptr = have_uvs ? &uvs : nullptr;

    if (s.base_frames_all) {
        // Level 0 is the cage and is never regional, so it has no ring outside
        // itself and passes none.
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
    level_normals_partial(lev.topology, c.conn, c.mesh.positions, s.scratch_c, &c.mesh.normals,
                          cross_level_of(s, level));
    lev.normals_pending.clear();
}

void evaluate_level0(MultiresSurface::State& s) {
    MultiresLevel& lev = s.levels[0];
    LevelCache& c = ensure_cache(s, 0);
    // The stack's contribution at level 0 first, because the cage's positions
    // are what the base frames — the transport frames for level 1 — are then
    // built from, and a proportion pass has to be under them.
    std::vector<std::uint32_t> recomposed;
    ensure_composed(s, 0, &recomposed);
    if (!c.evaluated) {
        gather_class_positions(s, &c.mesh.positions);
        apply_base_layers(s, nullptr);
        c.subdivided.clear();  // S(0) IS P(0); see LevelCache
        s.base_frames_all = true;
        c.evaluated = true;
        lev.note_moved_all();
    } else if (!recomposed.empty()) {
        apply_base_layers(s, &recomposed);
        // A base layer that moved has moved the CAGE the artist sees, so the
        // frames, the normals and every level above it follow — exactly as they
        // would for a level-0 stamp, because that is what this is.
        s.base_frames_dirty.insert(s.base_frames_dirty.end(), recomposed.begin(),
                                   recomposed.end());
        lev.note_moved(recomposed);
        mark_patches(s, 0, recomposed);
    }
    refresh_base_frames(s);
    drain_normals_pending(s, 0);
}

void full_evaluate(MultiresSurface::State& s, std::uint32_t level) {
    const MultiresLevel& parent = s.levels[level - 1];
    MultiresLevel& lev = s.levels[level];
    const LevelCache& pc = *parent.cache;
    LevelCache& c = *lev.cache;

    const ChildIndex stored = ChildIndex::of(lev.topology);
    subdivide_positions(parent.topology, pc.conn, pc.mesh.positions, stored, &c.subdivided);
    // The S-normals exist only to build the frames, which then carry them —
    // so they go into scratch rather than into a per-level array nobody would
    // read again.
    if (s.scratch_normals.size() < lev.topology.vertex_count)
        s.scratch_normals.resize(lev.topology.vertex_count, kernel::cf3(0, 1, 0));
    const CrossLevelNeighborhood* cross = cross_level_of(s, level);
    level_normals(lev.topology, c.conn, c.subdivided, &s.scratch_normals, cross);
    transport_frames(parent.topology, pc.conn, pc.frames, s.scratch_normals, stored, &c.frames);
    apply_detail_all(lev);
    level_normals(lev.topology, c.conn, c.mesh.positions, &c.mesh.normals, cross);
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
    const ChildIndex stored = ChildIndex::of(lev.topology);
    dirty_children(parent.topology, pc.conn, parent.pending(), stored, &s.scratch_a);
    // ...and the halo around them, whose normals — and therefore frames, and
    // therefore reconstructed detail — moved even though their subdivided
    // position did not.
    expand_by_face_ring(lev.topology, c.conn, s.scratch_a, &s.scratch_mark, &s.scratch_b);

    subdivide_positions_partial(parent.topology, pc.conn, pc.mesh.positions, stored, s.scratch_a,
                                &c.subdivided);
    if (s.scratch_normals.size() < lev.topology.vertex_count)
        s.scratch_normals.resize(lev.topology.vertex_count, kernel::cf3(0, 1, 0));
    const CrossLevelNeighborhood* cross = cross_level_of(s, level);
    level_normals_partial(lev.topology, c.conn, c.subdivided, s.scratch_b, &s.scratch_normals,
                          cross);
    transport_frames_partial(parent.topology, pc.conn, pc.frames, s.scratch_normals, stored,
                             s.scratch_b, &c.frames);
    apply_detail(lev, s.scratch_b);

    // The display normals reach one ring further again, because a vertex that
    // did not move still shades differently when its neighbour did.
    expand_by_face_ring(lev.topology, c.conn, s.scratch_b, &s.scratch_mark, &s.scratch_c);
    level_normals_partial(lev.topology, c.conn, c.mesh.positions, s.scratch_c, &c.mesh.normals,
                          cross);

    s.stats.vertices_evaluated += s.scratch_b.size();
    s.stats.normals_recomputed += s.scratch_c.size();
    ++s.stats.partial_level_updates;

    lev.note_moved(s.scratch_b);
    mark_patches(s, level, s.scratch_b);
}

// The vertices a recomposition moved, brought up to the state a stamp's
// vertices are in: position re-applied, display normals re-shaded one ring
// further, and queued for the level above.
//
// COSTS THE RECOMPOSED BLOCKS and nothing else, which is task 5.4's gate said
// in code: the list arrives from the composer, which walked the blocks the
// changed layer had allocated.
void reapply_recomposed(MultiresSurface::State& s, std::uint32_t level,
                        const std::vector<std::uint32_t>& vertices) {
    MultiresLevel& lev = s.levels[level];
    LevelCache& c = *lev.cache;
    apply_detail(lev, vertices);
    expand_by_face_ring(lev.topology, c.conn, vertices, &s.scratch_mark, &s.scratch_c);
    level_normals_partial(lev.topology, c.conn, c.mesh.positions, s.scratch_c, &c.mesh.normals,
                          cross_level_of(s, level));
    s.stats.vertices_evaluated += vertices.size();
    s.stats.normals_recomputed += s.scratch_c.size();
    lev.note_moved(vertices);
    mark_patches(s, level, vertices);
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

namespace {

// Is `target` already the surface it should be, with nothing below it waiting
// to be pushed up?
//
// WITHOUT THIS the walk from the cage is unconditional, and a level whose cache
// was RELEASED is rebuilt on the next touch even though nothing changed —
// which makes releasing the levels between the cage and the one being worked on
// pointless, because the next stamp brings them all back. A detail pass at a
// fine level reads that level's own subdivided positions and frames and nothing
// else, so when nothing below has moved there is nothing to walk.
//
// THE TARGET'S OWN PENDING NORMALS ARE NOT A REASON TO WALK. They are a
// recompute over the region the target was itself edited in, and the walk from
// the cage that would reach the same drain rebuilds every level under it — the
// exact cost the short circuit exists to avoid. `evaluate_up_to` drains them
// directly instead, which is why they are not tested here.
bool below_is_current(const MultiresSurface::State& s, std::uint32_t target) {
    if (!level_is_evaluated(s, target)) return false;
    // A COMPOSITION CHANGE IS PENDING WORK. Without this a strength change on a
    // hierarchy nobody has edited since is silently swallowed: nothing is
    // pending, every cache says it is evaluated, and the dial does nothing.
    if (composition_pending(s)) return false;
    if (s.base_frames_all || !s.base_frames_dirty.empty()) return false;
    for (std::uint32_t l = 0; l < target; ++l) {
        const MultiresLevel& lev = s.levels[l];
        if (lev.pending_all() || !lev.pending().empty() || !lev.normals_pending.empty())
            return false;
    }
    return true;
}

}  // namespace

void evaluate_up_to(MultiresSurface::State& s, std::uint32_t level) {
    if (s.levels.empty()) return;
    const std::uint32_t target = std::min(level, static_cast<std::uint32_t>(s.levels.size() - 1));
    if (below_is_current(s, target)) {
        // Nothing below moved, so the only work outstanding is whatever display
        // normals this level owes over its own last edit. That costs the
        // footprint; reaching it through the walk would cost the hierarchy.
        drain_normals_pending(s, target);
        return;
    }
    evaluate_level0(s);
    std::vector<std::uint32_t> recomposed;
    for (std::uint32_t l = 1; l <= target; ++l) {
        ensure_cache(s, l);
        // A level with no cache, a level that was never evaluated, and a level
        // whose parent changed everywhere are the same case: rebuild it whole,
        // and a whole rebuild reads the composed field so the recomposition
        // only has to happen first.
        if (!s.levels[l].cache->evaluated || s.levels[l - 1].pending_all()) {
            ensure_composed(s, l, nullptr);
            full_evaluate(s, l);
            s.levels[l].note_moved_all();
        } else {
            recomposed.clear();
            ensure_composed(s, l, &recomposed);
            if (!s.levels[l - 1].pending().empty()) partial_evaluate(s, l);
            // A block whose COMPOSITION changed moved vertices that nothing
            // below this level touched — a strength change on a layer that
            // lives here and nowhere else. Those vertices get the same
            // treatment a stamp's do: re-applied, re-shaded, and pushed to the
            // level above.
            if (!recomposed.empty()) reapply_recomposed(s, l, recomposed);
            drain_normals_pending(s, l);
        }
        s.levels[l - 1].clear_pending();
    }
}

void evaluate_all_up_to(MultiresSurface::State& s, std::uint32_t level) {
    if (s.levels.empty()) return;
    const std::uint32_t target = std::min(level, static_cast<std::uint32_t>(s.levels.size() - 1));
    // A missing cache is what `below_is_current` does not test for below the
    // target, so asking for that level directly is what forces the walk: it
    // fails the test on its own cache and rebuilds from the cage. Bit for bit
    // the same surface — every input to a level is still here, which is the
    // property `drop_all_caches` already rests on.
    //
    // AN ALLOCATED CACHE IS NOT AN EVALUATED ONE, and the two are one predicate
    // here rather than two cases. `ensure_cache` builds a level's connectivity
    // and nothing else — `connectivity_at` is a public call that reaches it, so
    // a host that asks a released level for its connectivity leaves the cache
    // holding a `conn` and an EMPTY position array. Testing only the pointer
    // walks past exactly that level, and the reader downstream indexes an empty
    // vector. `below_is_current` tests the flag for the same reason.
    for (std::uint32_t l = 0; l <= target; ++l)
        if (!level_is_evaluated(s, l)) evaluate_up_to(s, l);
    evaluate_up_to(s, target);
}

// -- attributes ---------------------------------------------------------------

namespace {

// Which attribute channels the CAGE carries. A hierarchy over a mesh with no
// colours exports none, so this is what decides whether there is anything to
// build at all.
struct AttrChannels {
    bool uvs = false;
    bool colors = false;
    bool any() const { return uvs || colors; }
};

AttrChannels channels_of(const MultiresSurface::State& s) {
    const std::size_t n = s.base.positions.size();
    AttrChannels c;
    c.uvs = !s.base.uvs.empty() && s.base.uvs.size() == n;
    c.colors = !s.base.colors.empty() && s.base.colors.size() == n;
    return c;
}

// The cage's own attribute level. Its connectivity is the RAW index buffer, so
// a seam's two duplicate vertices stay two vertices and the boundary rule
// interpolates each side of the seam along itself — which is the whole of "SHALL
// NOT average a UV across a seam", as a construction rather than a check.
bool build_attr_base(MultiresSurface::State& s, const AttrChannels& channels, AttrLevel* out) {
    if (s.attribute_split) {
        std::vector<std::uint32_t> identity(s.base.positions.size());
        for (std::size_t v = 0; v < identity.size(); ++v)
            identity[v] = static_cast<std::uint32_t>(v);
        if (!base_topology_from_mesh(s.base, identity.data(),
                                     static_cast<std::uint32_t>(identity.size()), &out->topology))
            return false;
        out->conn = LevelConnectivity::build(out->topology);
        out->to_geom = s.class_of;
    }
    if (channels.uvs) out->uvs = s.base.uvs;
    if (channels.colors) out->colors = s.base.colors;
    return true;
}

// One attribute level above another, built into a LOCAL and returned — so a
// reference into `s.attr` cannot outlive the reallocation that appending to it
// causes.
AttrLevel build_attr_level(MultiresSurface::State& s, std::uint32_t l) {
    AttrLevel a;
    const bool split = s.attribute_split;
    const LevelTopology& ptopo = split ? s.attr[l - 1].topology : s.levels[l - 1].topology;
    const LevelConnectivity& pconn = split ? s.attr[l - 1].conn : connectivity_of(s, l - 1);
    // A REGIONAL LEVEL'S ATTRIBUTES ARE REGIONAL TOO, over the same patches.
    // The two hierarchies emit faces in the same order, so the geometric
    // level's own `patch_kept` is the right restriction for the attribute one
    // and the face-for-face correspondence below still holds.
    const std::vector<char>& keep = s.levels[l].patch_kept;
    if (split) {
        a.topology = keep.empty() ? subdivide_topology(ptopo, pconn)
                                  : subdivide_topology_for_patches(ptopo, pconn, keep);
        a.conn = LevelConnectivity::build(a.topology);
        // The two hierarchies emit child faces in the same order — parent face,
        // then corner — so face f of one is face f of the other and the map
        // falls out of walking the corners side by side.
        const LevelTopology& gt = s.levels[l].topology;
        a.to_geom.assign(a.topology.vertex_count, 0u);
        const std::size_t corners = std::min(a.topology.corners.size(), gt.corners.size());
        for (std::size_t i = 0; i < corners; ++i) a.to_geom[a.topology.corners[i]] = gt.corners[i];
    }
    // Values against the attribute level's own numbering when there is one,
    // and against the geometric level's when the attributes ride on it.
    const ChildIndex stored =
        ChildIndex::of(split ? a.topology : s.levels[l].topology);
    if (!s.attr[l - 1].uvs.empty())
        subdivide_attribute(ptopo, pconn, s.attr[l - 1].uvs, stored, &a.uvs);
    if (!s.attr[l - 1].colors.empty())
        subdivide_attribute(ptopo, pconn, s.attr[l - 1].colors, stored, &a.colors);
    return a;
}

}  // namespace

bool ensure_attributes(MultiresSurface::State& s, std::uint32_t level) {
    const AttrChannels channels = channels_of(s);
    if (!channels.any() && !s.attribute_split) return false;
    if (s.attr.size() > level) return true;

    if (s.attr.empty()) {
        AttrLevel base;
        if (!build_attr_base(s, channels, &base)) return false;
        s.attr.push_back(std::move(base));
    }
    for (std::uint32_t l = static_cast<std::uint32_t>(s.attr.size());
         l <= level && l < s.levels.size(); ++l)
        s.attr.push_back(build_attr_level(s, l));
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

std::shared_ptr<const Adjacency> MultiresSurface::level_adjacency_shared(std::uint32_t level) {
    if (!state_ || !state_->level_ok(level)) return nullptr;
    Mesh& m = level_mesh(level);
    LevelCache& c = *state_->levels[level].cache;
    if (!c.adjacency) {
        // EXACT welding: a level's vertices are already the geometric points of
        // the surface, so an epsilon here would fuse a thin wall to itself for
        // no benefit. Two vertices that genuinely coincide bit for bit still
        // weld, which is what keeps a degenerate cage from cracking.
        c.adjacency = std::make_shared<const Adjacency>(Adjacency::build(m, 0.0f));
    }
    return c.adjacency;
}

const Adjacency& MultiresSurface::level_adjacency(std::uint32_t level) {
    static const Adjacency kEmpty;
    std::shared_ptr<const Adjacency> a = level_adjacency_shared(level);
    return a ? *a : kEmpty;
}

const CrossLevelNeighborhood& MultiresSurface::cross_level_at(std::uint32_t level) {
    static const CrossLevelNeighborhood kEmpty;
    if (!state_ || !state_->level_ok(level) || level == 0) return kEmpty;
    MultiresSurface::State& s = *state_;
    // The parent's evaluated positions are what the outside vertices are
    // subdivided from, so the walk up has to have happened.
    evaluate_up_to(s, level);
    LevelCache& c = *s.levels[level].cache;
    // THE LEVEL BELOW IS NOT ALWAYS RESIDENT. `evaluate_up_to` guarantees the
    // cache of the level it was ASKED for and no other, on purpose: when
    // nothing below has moved it short-circuits, which is the whole of what
    // makes `drop_intermediate_caches` stay dropped. So the parent's
    // connectivity and positions — which every outside vertex is subdivided
    // from — can be gone, and reading them through a released cache is the
    // undefined behaviour a sanitizer build catches here.
    //
    // A level that stores every child of every face of its parent has nothing
    // outside it and can say so without the parent at all — which is every
    // level of a uniform hierarchy, and why a trim there still holds.
    //
    // Anything else has to bring the parent back. Not for the topology, which
    // is fixed for the life of the cache, but for the OUTSIDE POSITIONS: they
    // belong to the level below, a stroke down there moves them without this
    // level's cache going stale, and the re-read on the way past is what keeps
    // them honest. Handing back what was last read would be an answer a reader
    // cannot tell from a current one, and handing back an empty neighbourhood
    // would read as "no depth boundary here".
    //
    // AND AN ALLOCATED CACHE IS NOT AN EVALUATED ONE: `ensure_cache` builds a
    // level's connectivity and leaves its positions empty, which `connectivity_at`
    // reaches from outside on a level a trim released. So the test is the flag
    // and not the pointer.
    if (!level_is_evaluated(s, level - 1)) {
        if (level_is_self_contained(s.levels[level].topology, s.levels[level].patch_kept))
            return kEmpty;
        evaluate_up_to(s, level - 1);
    }
    const MultiresLevel& parent = s.levels[level - 1];
    ++s.stats.cross_level_reads;
    if (!c.cross) {
        c.cross = std::make_unique<CrossLevelNeighborhood>(
            build_cross_level(parent.topology, parent.cache->conn, parent.cache->mesh.positions,
                              s.levels[level].topology, s.levels[level].patch_kept));
        c.cross_parent_revision = parent.positions_revision();
        return *c.cross;
    }
    // The topology is fixed for the life of the cache; the outside POSITIONS are
    // the level below's, and a stroke down there moves them without this level's
    // cache going stale. So they are re-read when — and only when — the level
    // below has moved since the last read.
    //
    // THE SIGNAL IS THE QUEUE THAT WAS ALREADY THERE. Those positions are
    // `subdivide_positions` of the parent's, so they go stale exactly when the
    // parent has vertices to push up; `MultiresLevel::note_moved` is the one
    // door onto that queue and moves `positions_revision` with it. Anything that
    // writes the parent's positions and skips that door has already left the
    // level above with stale `subdivided` and stale frames, which every existing
    // multires gate reads — so there is no way to make this stale on its own.
    //
    // A write the hierarchy then PUTS BACK deliberately does not move it:
    // `restore_positions` rewrites what the stored coefficients reconstruct to,
    // which is what the last revision left in the array, so the pair is a no-op
    // on the content and on the number. That is the coarse half of every
    // interior dab of a crossing stroke.
    if (c.cross_parent_revision != parent.positions_revision()) {
        ++s.stats.cross_level_refreshes;
        refresh_cross_level(parent.topology, parent.cache->conn, parent.cache->mesh.positions,
                            c.cross.get());
        c.cross_parent_revision = parent.positions_revision();
    }
    return *c.cross;
}

bool MultiresSurface::build_block(std::uint32_t level, std::uint32_t patch, Block* out) {
    if (!out || !state_ || !state_->level_ok(level)) return false;
    const LevelTopology& t = state_->levels[level].topology;
    if (patch >= t.patch_count && !(t.face_patch.empty() && patch < t.face_count)) return false;
    evaluate_up_to(*state_, level);

    out->patch = patch;
    out->level = level;
    out->vertices.clear();
    // CLEARED LIKE THE OTHER TWO, and for a reason a fresh block never shows:
    // a single-level block says every vertex is at `level` by leaving this
    // EMPTY, so a block reused after `build_mixed_block` would otherwise carry
    // the last patch's levels beside this patch's vertices — a host reading the
    // pair, as `Block`'s own comment tells it to, would read the wrong level's
    // positions and, where the arrays are different lengths, read off the end.
    out->vertex_levels.clear();
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

ExportWants export_wants(const MultiresSurface::State& s,
                        const MultiresExportOptions& options) {
    const std::size_t n = s.base.positions.size();
    ExportWants w;
    w.normals = options.normals && !s.base.normals.empty();
    w.uvs = options.uvs && s.base.uvs.size() == n && !s.base.uvs.empty();
    w.colors = options.colors && s.base.colors.size() == n && !s.base.colors.empty();
    return w;
}

namespace {

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

    const ExportWants wants = export_wants(*state_, options);
    const bool need_attrs = wants.uvs || wants.colors || state_->attribute_split;
    const bool have_attrs = need_attrs ? ensure_attributes(*state_, level) : false;

    if (!state_->attribute_split) {
        export_direct(*state_, level, wants, have_attrs, &out);
        return out;
    }
    // A SPLIT CAGE ALWAYS BUILDS ITS ATTRIBUTE CONNECTIVITY, and this is a
    // precondition rather than a fallback. `ensure_attributes` refuses only
    // when there is nothing to build, and it builds the raw-index topology from
    // the same faces the class-index one was built from — so if that succeeded
    // in `from_mesh`, this cannot fail. Falling back to the welded export would
    // hand back a mesh with a different vertex count and its UVs silently
    // missing, which is worse than the empty mesh a caller can test for.
    if (!have_attrs) return Mesh{};
    export_split(*state_, level, wants, &out);
    return out;
}

// -- editing ------------------------------------------------------------------

namespace {

// Put the cached positions back to what the STORED coefficients reconstruct to.
// What a refused write needs: the brush has already moved the level's mesh by
// the time the hierarchy is told about it, so refusing a locked layer means
// putting those vertices back rather than merely declining to record them.
//
// AND IT IS NOT A CHANGE, which is why it queues nothing for the level above
// and does not move `positions_revision`. Every acknowledged write to a level's
// positions ends by reading the stored coefficients back through the frame, so
// "what the coefficients reconstruct to" IS what the array held at the current
// revision -- the raw write this undoes never moved the number, and putting the
// values back leaves both where they were. Bumping here instead would cost a
// rim walk above for every interior dab of a crossing stroke, which restores
// the whole coarse write and keeps nothing.
void restore_positions(MultiresSurface::State& s, std::uint32_t level,
                       const std::vector<std::uint32_t>& vertices) {
    MultiresLevel& lev = s.levels[level];
    LevelCache& c = *lev.cache;
    if (level == 0) {
        const MultiresSurface::State::BaseRestFrames* rest = base_rest_frames(s);
        for (std::uint32_t v : vertices) {
            if (v >= s.class_count) continue;
            c.mesh.positions[v] = s.base.positions[s.class_members[s.class_offsets[v]]];
        }
        if (rest) apply_base_layers(s, &vertices);
        // AND THE DISPLAY NORMALS OVER THEM, which at level 0 travel with the
        // base frames -- exactly what `absorb_base_edit` queues for the write
        // this is the refusal of.
        s.base_frames_dirty.insert(s.base_frames_dirty.end(), vertices.begin(), vertices.end());
        return;
    }
    apply_detail(lev, vertices);
    // AND THE DISPLAY NORMALS, for the reason `absorb_level_edit` queues them
    // over what it absorbs. The caller moved this level's mesh, and whatever
    // recomputed its normals did so from the DISPLACED positions; putting the
    // positions back and leaving those normals is keeping something, which is
    // the one thing this call promises not to do. Drained over the region and
    // its face ring by `drain_normals_pending`.
    lev.normals_pending.insert(lev.normals_pending.end(), vertices.begin(), vertices.end());
}

// Is the write the caller is about to make refused because the layer it would
// land in is locked?
bool active_layer_locked(const MultiresSurface::State& s) {
    const SculptLayer* layer = s.stack.find(s.stack.active());
    return layer != nullptr && layer->locked;
}

// THE LAYERED WRITE PATH, and the one arithmetic rule the whole change is built
// around: it stores a DIFFERENCE, never a residual.
//
// Today's unlayered path stores `P_written − S(n)` whole. With a stack that
// would be wrong twice over — it would attribute the level's own base detail
// AND every other layer's contribution to the layer being written into. So the
// layered path reads what the composed field said BEFORE the stamp, expresses
// what the brush wrote in the same coefficients, and adds the difference:
//
//     ΔE = frame⁻¹(P_written) − E_before          (the frames do not move
//     L_active(v) += ΔE                            inside one stamp)
//
// which is exactly the displacement the brush applied, recorded AT FULL SIZE
// whatever the layer's strength is. That is requirement 3.2, and the visible
// consequence is stated rather than hidden: sculpting on a layer at strength
// 0.5 moves the surface by half of what the pen asked for, and raising the
// strength to 1 afterwards doubles it. That is the only reading under which "no
// work was lost" is true, and NOTHING here divides by a strength — the
// alternative, scaling the pen up by 1/s so the surface tracks the cursor, is
// undefined at exactly the value one slider reaches.
void absorb_layered_detail(MultiresSurface::State& s, std::uint32_t level,
                           const std::vector<std::uint32_t>& vertices) {
    MultiresLevel& lev = s.levels[level];
    LevelCache& c = *lev.cache;
    const SculptLayerId active = s.stack.active();
    const std::uint32_t bs = s.stack.block_size();

    for (std::uint32_t v : vertices) {
        if (v >= lev.topology.vertex_count) continue;
        const LocalDetail before = effective_detail(lev).get(v);
        const kernel::cfloat3 offset = c.mesh.positions[v] - c.subdivided[v];
        LocalDetail written;
        world_to_frame(c.frames[v], offset, &written.tangent, &written.bitangent,
                       &written.normal);
        LocalDetail delta;
        delta.tangent = written.tangent - before.tangent;
        delta.bitangent = written.bitangent - before.bitangent;
        delta.normal = written.normal - before.normal;
        if (active != kNoSculptLayer) {
            s.stack.add_detail(active, level, v, delta);
            continue;
        }
        // No active layer, but the level carries a stack: the difference goes
        // into the BASE, which is what "sculpt the form under the passes"
        // means.
        LocalDetail base = lev.detail.get(v);
        base.tangent += delta.tangent;
        base.bitangent += delta.bitangent;
        base.normal += delta.normal;
        lev.detail.set(v, base);
        s.stack.invalidate(level, v / bs);
    }
    // AND READ IT BACK from the recomposed field, for the reason the unlayered
    // path reads back from its own: the stored coefficients are authoritative,
    // so the cached position has to be what they reconstruct to rather than
    // what the brush wrote — the two differ by the last bits of a round trip
    // through the frame, and a redo would otherwise land on neither.
    ensure_composed(s, level, nullptr);
    apply_detail(lev, vertices);
}

// A stamp at level 0 on a hierarchy carrying base deformation layers. The cage
// stays the FORM: what the brush added over the layer's own contribution is
// either the layer's (an active layer) or the cage's (none), and either way the
// contribution is subtracted out rather than baked in.
void absorb_base_edit(MultiresSurface::State& s, const std::vector<std::uint32_t>& vertices) {
    LevelCache& c = *s.levels[0].cache;
    const SculptLayerId active = s.stack.active();
    const bool layered = s.levels[0].composed != nullptr;
    const MultiresSurface::State::BaseRestFrames* rest = layered ? base_rest_frames(s) : nullptr;
    const std::uint32_t bs = s.stack.block_size();

    for (std::uint32_t v : vertices) {
        if (v >= s.class_count) continue;
        if (rest && v < rest->frames.size()) {
            const LocalDetail before = s.levels[0].composed->get(v);
            LocalDetail written;
            world_to_frame(rest->frames[v], c.mesh.positions[v] - rest->positions[v],
                           &written.tangent, &written.bitangent, &written.normal);
            if (active != kNoSculptLayer) {
                LocalDetail delta;
                delta.tangent = written.tangent - before.tangent;
                delta.bitangent = written.bitangent - before.bitangent;
                delta.normal = written.normal - before.normal;
                s.stack.add_detail(active, 0, v, delta);
                continue;
            }
            // The CAGE takes the difference, with the layer's contribution
            // subtracted out — otherwise sculpting the form under a proportion
            // pass would bake that pass into the cage and the slider would stop
            // meaning anything.
            const kernel::cfloat3 contribution = frame_to_world(
                rest->frames[v], before.tangent, before.bitangent, before.normal);
            const kernel::cfloat3 form = c.mesh.positions[v] - contribution;
            for (std::uint32_t i = s.class_offsets[v]; i < s.class_offsets[v + 1]; ++i)
                s.base.positions[s.class_members[i]] = form;
            s.stack.invalidate(0, v / bs);
            continue;
        }
        // The cage is authoritative: what the brush wrote IS the geometry, and
        // it is copied to every raw vertex of the class so a seam's duplicates
        // stay coincident and cannot open into a crack.
        const kernel::cfloat3 p = c.mesh.positions[v];
        for (std::uint32_t i = s.class_offsets[v]; i < s.class_offsets[v + 1]; ++i)
            s.base.positions[s.class_members[i]] = p;
    }
    if (s.base_rest) s.base_rest->valid = false;
    if (layered) {
        ensure_composed(s, 0, nullptr);
        apply_base_layers(s, &vertices);
    }
    s.base_frames_dirty.insert(s.base_frames_dirty.end(), vertices.begin(), vertices.end());
    ++s.base_revision;
}

}  // namespace

void MultiresSurface::absorb_level_edit(std::uint32_t level,
                                        const std::vector<std::uint32_t>& vertices) {
    if (!state_ || !state_->level_ok(level) || vertices.empty()) return;
    evaluate_up_to(*state_, level);
    MultiresLevel& lev = state_->levels[level];
    LevelCache& c = *lev.cache;

    // A LOCKED LAYER REFUSES THE WRITE, and refusing means putting the level's
    // mesh back: the brush moved it before the hierarchy was told, so declining
    // to record would leave a cached surface that no stored coefficient
    // reconstructs.
    if (active_layer_locked(*state_)) {
        restore_positions(*state_, level, vertices);
        return;
    }

    const bool layered = lev.composed != nullptr;
    const bool into_layer = state_->stack.active() != kNoSculptLayer;
    if (level == 0) {
        absorb_base_edit(*state_, vertices);
    } else if (layered || into_layer) {
        absorb_layered_detail(*state_, level, vertices);
        ++state_->detail_revision;
    } else {
        // THE UNLAYERED PATH, unchanged bit for bit. Above the cage the surface
        // is the subdivision plus a residual, so what is stored is the
        // residual: the difference between where the brush left the vertex and
        // where the subdivision would have put it, read in the transported
        // frame.
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
        // AND THE DISPLAY NORMALS THIS LEVEL OWNS, over the region and its
        // ring, which is what `drain_normals_pending` does with this list.
        //
        // WHY THE CALLER'S NORMALS ARE NOT ENOUGH, even when the caller is
        // `MeshSculptor` and has just recomputed them. Two things write
        // `c.mesh.normals` and they do not agree: the sculptor derives them
        // from the level mesh's TRIANGLES, and everything in this file derives
        // them from the level's own faces by Newell — which on a subdivision
        // quad is a different vector, measured at about a degree over a
        // sculpted region. So a hierarchy that had been sculpted shaded one way
        // while its cache was warm and another way after a `drop_all_caches`
        // rebuilt it, which is a visible change under memory pressure with no
        // edit behind it and nothing in the surface to explain it. The
        // hierarchy is authoritative about its own normals; the level mesh's
        // are an input to the deformation, not the answer.
        //
        // The second reason is smaller and would have been enough on its own:
        // the loop above REWROTE the positions these normals were computed
        // from, reading the coefficients back through the frame, so even one
        // definition would have left them a round trip behind.
        lev.normals_pending.insert(lev.normals_pending.end(), vertices.begin(), vertices.end());
        ++state_->detail_revision;
    }
    lev.note_moved(vertices);
    mark_patches(*state_, level, vertices);
    ++state_->evaluated_revision;
}

void MultiresSurface::restore_level_positions(std::uint32_t level,
                                              const std::vector<std::uint32_t>& vertices) {
    if (!state_ || !state_->level_ok(level) || vertices.empty()) return;
    // EVALUATED FIRST, for the same reason `absorb_level_edit` evaluates first:
    // "what the stored coefficients reconstruct to" is a statement about
    // `subdivided` and `frames`, and a level whose parent has moved does not
    // have those yet.
    evaluate_up_to(*state_, level);
    // FILTERED, because `restore_positions` indexes the caches directly: it is
    // reached from `absorb_level_edit`, which has already dropped the
    // out-of-range ids, and this entry point has not.
    const std::uint32_t count =
        level == 0 ? state_->class_count : state_->levels[level].topology.vertex_count;
    std::vector<std::uint32_t> in_range;
    in_range.reserve(vertices.size());
    for (std::uint32_t v : vertices)
        if (v < count) in_range.push_back(v);
    if (!in_range.empty()) restore_positions(*state_, level, in_range);
}

void MultiresSurface::set_detail(std::uint32_t level, std::uint32_t vertex,
                                 const LocalDetail& value) {
    if (!state_ || !state_->level_ok(level) || level == 0) return;
    MultiresLevel& lev = state_->levels[level];
    if (vertex >= lev.topology.vertex_count) return;
    lev.detail.set(vertex, value);
    if (lev.composed) {
        // THE BASE BENEATH THE STACK MOVED, so the composed field is stale in
        // this block — and the position is deliberately NOT written here. The
        // next evaluation recomposes the block and re-applies its positions and
        // its display normals together; writing one now, from a composed value
        // that has not been recomposed yet, would leave a cached position that
        // no stored coefficient reconstructs. Not a layer change, so no layer
        // revision moves.
        state_->stack.invalidate(level, vertex / state_->stack.block_size());
    } else if (lev.cache && lev.cache->evaluated) {
        LevelCache& c = *lev.cache;
        c.mesh.positions[vertex] =
            c.subdivided[vertex] +
            frame_to_world(c.frames[vertex], value.tangent, value.bitangent, value.normal);
        lev.normals_pending.push_back(vertex);
    }
    lev.note_moved(vertex);
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
    // The cage moved, so the REST frames a base deformation layer is measured
    // in moved with it and have to be rebuilt before the next read.
    if (state_->base_rest) state_->base_rest->valid = false;
    if (lev.cache && lev.cache->evaluated) {
        lev.cache->mesh.positions[vertex] = position;
        if (lev.composed) {
            const std::vector<std::uint32_t> one{vertex};
            apply_base_layers(*state_, &one);
        }
    }
    state_->base_frames_dirty.push_back(vertex);
    lev.note_moved(vertex);
    lev.normals_pending.push_back(vertex);
    mark_patches(*state_, 0, {vertex});
    ++state_->base_revision;
    ++state_->evaluated_revision;
}

}  // namespace mesh
}  // namespace clay
