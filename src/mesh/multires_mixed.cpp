// THE MIXED-DEPTH EXPORT: one surface out of a hierarchy of several depths
// (mesh-multires spec, finish-regional-multires).
//
// A regional level stores the faces of the base patches it refines and nothing
// else, so there is no single level that IS the model. A host that follows the
// instruction the display API gives it -- `build_block(effective_level(patch,
// display), patch)` per patch, welded by position -- gets a surface with 240
// boundary edges on a closed cage at display 3, against 0 for one level
// everywhere. Two defects, and the smaller one is the T-junction: the fine
// side's edge point sits 0.0232 off the coarse chord, while the SHARED CAGE
// VERTEX is 0.0434 apart across a 0.5 cage edge, nearly twice as much.
//
// WHAT THIS FILE DOES ABOUT IT, and it is one rule rather than a table of
// configurations. Emit each base patch at its own depth, and ask two questions
// of the level above about every corner and every edge of every face emitted:
//
//     does the level above store this vertex's VERTEX POINT?
//         then the corner is that vertex, at that level.
//     does the level above store this edge's EDGE POINT?
//         then that point goes into the corner list after the corner it
//         follows.
//
// Both questions are about the level above alone, so two faces that share an
// element get the same answer whichever side they are emitted from, and the
// emitted mesh is watertight BY IDENTITY -- the shared element is one index
// carrying one set of bits. Nothing is welded, nothing is compared to a
// tolerance, and there is no configuration to enumerate: the corner-only case,
// the 1-, 2-, 3- and 4-sided cases and the interior case are the same two
// questions with different answers.
//
// WHY THE FINE SIDE WINS AT A SHARED VERTEX. Its value is one subdivision step
// closer to the limit, and it is the value the fine patch's own faces already
// carry -- adopting the coarse one would mean rewriting the refined region.
// The consequence is the RIPPLE, and it is the half the task wording
// undercounts: every coarse face incident to that vertex must take the same
// value, including a face that meets the fine region only at a corner and has
// no split edge to fix. Emitting the corner through the level above rather than
// through a "is this face a transition" test is what makes the ripple automatic
// instead of a case to remember.
//
// ONE LEVEL, ALWAYS. `resolve_keep` refuses to refine a patch unless it and its
// whole vertex ring are resident one level down, which is stricter than 2:1, so
// no two patches sharing a cage vertex differ by more than one level. That is
// why each question above is asked of ONE level above and not of a chain: a
// second step could only be reached from a patch two levels away from a patch
// it shares a vertex with, which the hierarchy cannot build.
//
// WHAT IT COSTS IN QUADS, which is arithmetic and not a shortcut. A coarse quad
// with one split edge is a pentagon, and a polygon with an odd number of
// boundary vertices has no quadrangulation at all: four edges per quad counts
// every interior edge twice, so 4q = b + 2i_edges and b must be even, whatever
// is added inside. Making b even would mean splitting a second edge of that
// face, whose midpoint is then a vertex the neighbouring coarse face must also
// carry, and so on out of the transition and across the model. So the export is
// quad-clean exactly when no edge is split, and a triangle list otherwise --
// which `level_faces_into` already decides for us, and `mesh_data.h` already
// forbids lying about.
//
// NOTHING HERE IS STORED. The emitted topology is a function of the cage, the
// rule and the per-level patch sets, and a version-3 stream already carries all
// three; `multires_serialize.cpp` writes the patch sets and replays the build.
// So there are no bytes, no `kSurfaceVersion` bump, and the spec's line about
// transition geometry never becoming the authoritative sculpt representation is
// structural rather than a discipline someone has to keep.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "clay/mesh/multires.h"
#include "clay/mesh/subdivide.h"
#include "clay/mesh/surface_frame.h"
#include "multires_internal.h"

namespace clay {
namespace mesh {
namespace {

using kernel::cfloat3;

constexpr std::uint32_t kAllPatches = 0xffffffffu;

// A vertex of the emitted surface, before it is numbered: the level it lives at
// and its index there. One integer so the whole set sorts, and the sort IS the
// output numbering -- ascending level, then ascending vertex, which for a
// uniform-depth export is exactly the level's own numbering and therefore
// exactly `mesh_at_level`'s.
std::uint64_t joined_id(std::uint32_t level, std::uint32_t vertex) {
    return (static_cast<std::uint64_t>(level) << 32) | vertex;
}
std::uint32_t id_level(std::uint64_t id) { return static_cast<std::uint32_t>(id >> 32); }
std::uint32_t id_vertex(std::uint64_t id) { return static_cast<std::uint32_t>(id); }

// The two questions this whole file is, asked of the level above a face's own.
//
// `present` is false at the display level, where there is no level above to ask
// and every answer is therefore "this level's own".
struct AboveIndex {
    bool present = false;
    ChildIndex above;
    std::uint32_t edge_base = 0;

    // The corner's identity in the emitted mesh: the level above's vertex point
    // when it holds one -- which is what makes a shared cage vertex ONE index
    // carrying the fine side's value -- and this level's own vertex otherwise.
    std::uint64_t corner(std::uint32_t level, std::uint32_t v) const {
        const std::uint32_t up = present ? above.stored(v) : kNoVertex;
        return up == kNoVertex ? joined_id(level, v) : joined_id(level + 1, up);
    }

    // The point the level above put on this edge, in that level's numbering,
    // or `kNoVertex` when it put none. The T-junction, and the only thing that
    // makes an emitted face something other than a quad.
    std::uint32_t split(std::uint32_t edge) const {
        return present ? above.stored(edge_base + edge) : kNoVertex;
    }
};

// The emitted polygons, in joined ids, before they are numbered.
struct MixedFaces {
    std::vector<std::uint64_t> corners;  // face after face
    std::vector<std::uint32_t> offsets;  // face_count + 1
    std::vector<std::uint32_t> patch;    // the base patch each face belongs to
    bool all_quads = true;
};

// `min(display, patch_max_level(patch))` without the walk over every level: the
// deepest level at or below `display` that keeps this patch.
std::uint32_t depth_of(const MultiresSurface::State& s, std::uint32_t patch,
                       std::uint32_t display) {
    for (std::uint32_t l = display + 1; l-- > 1;)
        if (s.levels[l].keeps(patch)) return l;
    return 0;
}

std::vector<std::uint32_t> patch_depths(const MultiresSurface::State& s, std::uint32_t display) {
    const std::uint32_t patches = s.levels[0].topology.face_count;
    std::vector<std::uint32_t> depth(patches, 0u);
    for (std::uint32_t p = 0; p < patches; ++p) depth[p] = depth_of(s, p, display);
    return depth;
}

// One face's polygon: its corners, each resolved to the level above where that
// level holds them, with the fine edge point of every split edge inserted after
// the corner it follows.
//
// The corner order is the face's own, so a quad with nothing to bridge comes
// back as the same quad in the same order it was stored in.
void emit_face(const LevelTopology& t, const LevelConnectivity& conn, const AboveIndex& above,
               std::uint32_t level, std::uint32_t f, std::vector<std::uint64_t>* corners) {
    std::uint32_t arity = 0;
    const std::uint32_t* c = t.face(f, &arity);
    const std::uint32_t begin = t.face_begin(f);
    for (std::uint32_t i = 0; i < arity; ++i) {
        corners->push_back(above.corner(level, c[i]));
        const std::uint32_t split = above.split(conn.corner_edge[begin + i]);
        if (split != kNoVertex) corners->push_back(joined_id(level + 1, split));
    }
}

// What the level above `l` holds, or nothing at the display level -- where
// there is no level above and every face is emitted as it is stored.
AboveIndex above_of(MultiresSurface::State& s, std::uint32_t l, std::uint32_t display,
                    const LevelTopology& t, const LevelConnectivity& conn) {
    AboveIndex above;
    if (l >= display) return above;
    above.present = true;
    above.above = ChildIndex::of(s.levels[l + 1].topology);
    above.edge_base = ChildLayout::of(t, conn).edge_base;
    return above;
}

// The faces ONE level contributes: those whose patch is emitted at this level,
// in the level's own face order.
void collect_level_faces(const LevelTopology& t, const LevelConnectivity& conn,
                         const AboveIndex& above, std::uint32_t level,
                         const std::vector<std::uint32_t>& depth, std::uint32_t only,
                         MixedFaces* out) {
    for (std::uint32_t f = 0; f < t.face_count; ++f) {
        const std::uint32_t p = t.patch_of(f);
        if (p >= depth.size() || depth[p] != level) continue;
        if (only != kAllPatches && p != only) continue;
        emit_face(t, conn, above, level, f, &out->corners);
        out->offsets.push_back(static_cast<std::uint32_t>(out->corners.size()));
        out->patch.push_back(p);
    }
}

// Every face of the mixed-depth surface, or of one patch of it.
//
// LEVEL-MAJOR, then the level's own face order, which is patch-major in the
// parent's order. Deterministic without a new rule -- `full_of` is ascending
// and the face order is the parent's -- and, on a hierarchy whose patches all
// reach `display`, exactly that level's face list.
void collect_faces(MultiresSurface::State& s, std::uint32_t display,
                   const std::vector<std::uint32_t>& depth, std::uint32_t only,
                   MixedFaces* out) {
    out->offsets.assign(1, 0u);
    for (std::uint32_t l = 0; l <= display; ++l) {
        if (only != kAllPatches && depth[only] != l) continue;
        const LevelTopology& t = s.levels[l].topology;
        const LevelConnectivity& conn = connectivity_of(s, l);
        collect_level_faces(t, conn, above_of(s, l, display, t, conn), l, depth, only, out);
    }
    for (std::size_t i = 1; i < out->offsets.size(); ++i)
        if (out->offsets[i] - out->offsets[i - 1] != 4u) out->all_quads = false;
}

// The joined ids the faces use, ascending and free of duplicates. This IS the
// output vertex numbering: the same two passes and the same binary search
// `build_block` uses, for the same reason -- the order is then a function of
// the topology alone and two callers asking the same question get the same
// buffer.
std::vector<std::uint64_t> number_vertices(const MixedFaces& faces) {
    std::vector<std::uint64_t> out = faces.corners;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::uint32_t local_of(const std::vector<std::uint64_t>& vertices, std::uint64_t id) {
    return static_cast<std::uint32_t>(
        std::lower_bound(vertices.begin(), vertices.end(), id) - vertices.begin());
}

// The emitted polygons as a `LevelTopology`, so everything downstream -- the
// triangulation, the quad list and the invariant that ties them together -- is
// the code the single-level export already runs through.
LevelTopology mixed_topology(const MixedFaces& faces, const std::vector<std::uint64_t>& vertices,
                             std::uint32_t patch_count) {
    LevelTopology t;
    t.vertex_count = static_cast<std::uint32_t>(vertices.size());
    t.face_count = static_cast<std::uint32_t>(faces.patch.size());
    t.patch_count = patch_count;
    t.face_patch = faces.patch;
    t.corners.resize(faces.corners.size());
    for (std::size_t i = 0; i < faces.corners.size(); ++i)
        t.corners[i] = local_of(vertices, faces.corners[i]);
    // `face_offsets` EMPTY means quads (see `subdivide.h`), and that is the
    // whole quad decision: a surface with no split edge is uniform quads and
    // keeps `Mesh::quads`, and one pentagon takes it away from the whole mesh
    // because a quad list that does not describe `indices` is the lie
    // `mesh_data.h` forbids.
    if (!faces.all_quads) t.face_offsets = faces.offsets;
    return t;
}

void gather_positions(MultiresSurface::State& s, const std::vector<std::uint64_t>& vertices,
                      std::vector<cfloat3>* out) {
    out->resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i)
        (*out)[i] = s.levels[id_level(vertices[i])].cache->mesh.positions[id_vertex(vertices[i])];
}

// One attribute channel, gathered per emitted vertex from the level that vertex
// lives at. Only reached on a cage whose attribute connectivity IS its
// geometric one -- a split cage is refused, because its attribute topology has
// no face-for-face counterpart for a mixed-depth face -- so `s.attr[l]` is
// indexed by the level's own vertices and needs no map.
template <typename T>
void gather_attribute(const std::vector<AttrLevel>& attr, std::vector<T> AttrLevel::*channel,
                      const std::vector<std::uint64_t>& vertices, std::vector<T>* out) {
    out->assign(vertices.size(), T{});
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const std::vector<T>& src = attr[id_level(vertices[i])].*channel;
        if (id_vertex(vertices[i]) < src.size()) (*out)[i] = src[id_vertex(vertices[i])];
    }
}

void gather_attributes(MultiresSurface::State& s, std::uint32_t display, const ExportWants& wants,
                       const std::vector<std::uint64_t>& vertices, Mesh* out) {
    if (!wants.attributes() || !ensure_attributes(s, display)) return;
    if (s.attr.size() <= display) return;
    if (wants.uvs) gather_attribute(s.attr, &AttrLevel::uvs, vertices, &out->uvs);
    if (wants.colors) gather_attribute(s.attr, &AttrLevel::colors, vertices, &out->colors);
}

}  // namespace

Mesh MultiresSurface::mixed_mesh_at_level(std::uint32_t level,
                                          const MultiresExportOptions& options,
                                          MultiresMixedStatus* out_status,
                                          const parallel::CancelToken* cancel) {
    const auto refuse = [&](MultiresMixedStatus status) {
        if (out_status) *out_status = status;
        return Mesh{};
    };
    if (!state_ || !state_->level_ok(level)) return refuse(MultiresMixedStatus::NotBuilt);
    State& s = *state_;
    const ExportWants wants = export_wants(s, options);
    // Refused BEFORE the evaluation, so a caller that asked the impossible pays
    // nothing for the answer.
    if (s.attribute_split && wants.attributes())
        return refuse(MultiresMixedStatus::AttributeSplitCage);

    evaluate_up_to(s, level);
    // A cancelled export returns an EMPTY mesh rather than a partial one, for
    // the reason `mesh_at_level` gives: a caller that ignored the cancel and
    // drew the result would draw a fraction of the model.
    if (cancel && cancel->cancelled()) return refuse(MultiresMixedStatus::Cancelled);

    MixedFaces faces;
    collect_faces(s, level, patch_depths(s, level), kAllPatches, &faces);
    const std::vector<std::uint64_t> vertices = number_vertices(faces);
    const LevelTopology topology =
        mixed_topology(faces, vertices, s.levels[0].topology.face_count);

    Mesh out;
    std::vector<cfloat3> positions;
    gather_positions(s, vertices, &positions);
    level_to_mesh(topology, positions, &out);
    if (wants.normals) {
        // OVER THE ASSEMBLED MESH, not copied from either level. A vertex on
        // the seam has faces from both sides here and neither level's normal
        // array is a statement about this mesh; on a uniform-depth hierarchy
        // this topology IS the level, so the answer is the level's own bits.
        const LevelConnectivity conn = LevelConnectivity::build(topology);
        level_normals(topology, conn, out.positions, &out.normals);
    }
    gather_attributes(s, level, wants, vertices, &out);
    if (out_status) *out_status = MultiresMixedStatus::Ok;
    return out;
}

bool MultiresSurface::build_mixed_block(std::uint32_t level, std::uint32_t patch, Block* out) {
    if (!out || !state_ || !state_->level_ok(level)) return false;
    State& s = *state_;
    if (patch >= s.levels[0].topology.face_count) return false;
    evaluate_up_to(s, level);

    const std::vector<std::uint32_t> depth = patch_depths(s, level);
    MixedFaces faces;
    collect_faces(s, level, depth, patch, &faces);
    const std::vector<std::uint64_t> vertices = number_vertices(faces);

    out->patch = patch;
    out->level = depth[patch];
    out->vertices.clear();
    out->vertex_levels.clear();
    out->indices.clear();
    bool spans_levels = false;
    for (std::uint64_t id : vertices) {
        out->vertices.push_back(id_vertex(id));
        spans_levels = spans_levels || id_level(id) != out->level;
    }
    // EMPTY UNLESS IT SPANS TWO LEVELS, so a block away from a depth boundary
    // is byte for byte what `build_block` returns and a host that never looks
    // at this array reads what it always read.
    if (spans_levels)
        for (std::uint64_t id : vertices) out->vertex_levels.push_back(id_level(id));

    for (std::uint32_t f = 0; f + 1 < faces.offsets.size(); ++f) {
        const std::uint32_t begin = faces.offsets[f], end = faces.offsets[f + 1];
        for (std::uint32_t i = begin + 2; i < end; ++i)
            out->indices.insert(out->indices.end(),
                                {local_of(vertices, faces.corners[begin]),
                                 local_of(vertices, faces.corners[i - 1]),
                                 local_of(vertices, faces.corners[i])});
    }
    return true;
}

}  // namespace mesh
}  // namespace clay
