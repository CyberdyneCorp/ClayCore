#pragma once

// CATMULL-CLARK SUBDIVISION, as a deterministic topological relationship
// (mesh-multires spec, add-mesh-multires).
//
// This file answers one question and refuses the others: given a level, what
// is the level above it, and where do its vertices sit when nothing has been
// sculpted on them? It knows nothing about detail, nothing about frames and
// nothing about brushes. `multires.h` is what stacks these into a hierarchy.
//
// WHY THE RULE MATTERS MORE THAN THE ARITHMETIC. Catmull-Clark is four
// well-known averages and half a page of code. What is NOT free is the
// requirement the spec puts above them: *the same base mesh and the same rule
// SHALL produce the same hierarchy on every platform and every run*. A
// generated vertex's INDEX is its identity — detail is stored against it, undo
// records it, a saved document restores it, and a host's uploaded buffer is
// laid out by it — so an implementation that numbers its edge points by walking
// an `unordered_map` produces a different file on a different libstdc++ and
// silently reattaches every wrinkle to the wrong place. Hence:
//
//     [0, Nv)          vertex points, in PARENT VERTEX order
//     [Nv, Nv+Ne)      edge points,   in CANONICAL EDGE order (min, max), sorted
//     [Nv+Ne, +Nf)     face points,   in PARENT FACE order
//
// and child faces in parent-face order, then corner order. Nothing is hashed
// into existence. The consequence is worth stating because it removes a whole
// data structure: a child vertex's parentage is DERIVABLE FROM ITS INDEX
// (`origin_of` below), so no per-vertex parent record is stored at any level.
//
// FACE ARITY. Level 0 is whatever the base cage is — quads (`Mesh::quads`),
// triangles, or the mixture an importer produced. Every level above it is PURE
// QUADS, because that is what Catmull-Clark does to any face: an n-gon becomes
// n quads. That is not a convenience, it is why this rule was chosen over Loop
// for the first implementation — an imported retopologised character is quads,
// `mesh::Mesh` already carries them beside the triangles, and one face arity
// above level 0 is one code path in everything that reads a level.
//
// WHAT IS AUTHORITATIVE AND WHAT IS DERIVED, which is the memory decision this
// change had to make. `LevelTopology` — the corner list and the base-patch id
// per face — is kept per level. `LevelConnectivity` — edges, the corner->edge
// map, and the vertex incidences — is REBUILT from it on demand and thrown
// away, because it is roughly four times the size of the thing it is derived
// from and is only needed for the levels actually in use. See
// `multires.h`'s runtime cache.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"

namespace clay {
namespace mesh {

struct Mesh;

// The subdivision rule a hierarchy was built with. RECORDED rather than
// assumed, in the file format and across the ABI: a hierarchy reconstructed
// with a different rule than it was authored with is a different surface, and
// nothing else in the stream reveals the substitution.
enum class SubdivisionRule : std::uint8_t {
    CatmullClark = 0,
};

inline constexpr std::uint32_t kNoFace = 0xffffffffu;
inline constexpr std::uint32_t kNoEdge = 0xffffffffu;
inline constexpr std::uint32_t kNoVertex = 0xffffffffu;

// -- one level's connectivity -------------------------------------------------

// The faces of one level, as a corner list.
//
// Vertices here are GEOMETRIC POINTS, not the base mesh's raw indices: level 0
// is built over `Adjacency`'s weld classes, so a UV seam's two duplicate
// vertices are one point and a walk crosses the seam. What keeps the seam's two
// UVs apart is a SECOND topology over the raw indices, built only when the base
// carries attributes — see `multires.h`.
struct LevelTopology {
    std::uint32_t vertex_count = 0;

    // Corner indices, face after face. For level 0 the arity varies and
    // `face_offsets` says where each face starts; above level 0 every face is a
    // quad and `face_offsets` is EMPTY, because storing 4*f would cost four
    // bytes a face to say what the rule already guarantees.
    std::vector<std::uint32_t> corners;
    std::vector<std::uint32_t> face_offsets;  // empty, or face_count + 1
    std::uint32_t face_count = 0;

    // The LEVEL-0 face every face descends from. The chunk identity a host
    // uploads by and the unit dirty propagation is reported in: a base face
    // owns a subtree, that subtree never moves between faces, and its id is
    // therefore stable for the life of the hierarchy. Empty at level 0, where
    // it is the identity.
    std::vector<std::uint32_t> face_patch;
    std::uint32_t patch_count = 0;

    bool uniform_quads() const { return face_offsets.empty(); }

    std::uint32_t face_begin(std::uint32_t f) const {
        return uniform_quads() ? f * 4u : face_offsets[f];
    }
    std::uint32_t face_arity(std::uint32_t f) const {
        return uniform_quads() ? 4u : face_offsets[f + 1] - face_offsets[f];
    }
    const std::uint32_t* face(std::uint32_t f, std::uint32_t* out_arity) const {
        *out_arity = face_arity(f);
        return corners.data() + face_begin(f);
    }
    std::uint32_t patch_of(std::uint32_t f) const { return face_patch.empty() ? f : face_patch[f]; }

    std::size_t corner_count() const { return corners.size(); }
    // What this level costs to KEEP. The figure `MultiresMemory::topology`
    // sums, and the one `MultiresPreflight` predicts before a level exists.
    std::size_t bytes() const;
};

// An undirected edge with the faces on either side. `f1 == kNoFace` is an open
// boundary, which the subdivision rules branch on and must never guess at.
struct LevelEdge {
    std::uint32_t a = 0, b = 0;  // a < b, always
    std::uint32_t f0 = kNoFace, f1 = kNoFace;

    bool boundary() const { return f1 == kNoFace; }
};

// Everything derived from a `LevelTopology`: the edges, which edge each corner
// walks along, and the vertex incidences the vertex rule reads.
//
// DERIVED AND DROPPABLE. Nothing here is authoritative — rebuild it and you get
// the same bytes, which `MultiresSurface` relies on when it releases the caches
// of levels nobody is looking at. It is roughly four times the size of the
// topology it comes from, so keeping it for every level of a deep hierarchy is
// exactly the memory mistake this change exists to avoid.
struct LevelConnectivity {
    // Sorted by (a, b), which IS the canonical order the edge-point numbering
    // depends on. Not a hash map, not insertion order.
    std::vector<LevelEdge> edges;

    // One entry per CORNER, parallel to `LevelTopology::corners`: the edge
    // leading from that corner to the next corner of the same face.
    std::vector<std::uint32_t> corner_edge;

    // CSR incidences. `vertex_faces` is what the vertex rule averages face
    // points over; `vertex_edges` is what it averages edge midpoints over and
    // what says whether a vertex is on the boundary.
    std::vector<std::uint32_t> vertex_face_offsets, vertex_faces;
    std::vector<std::uint32_t> vertex_edge_offsets, vertex_edges;

    // Three or more faces met on one edge. The subdivision rules have no
    // meaning there — "the two faces beside this edge" is not a question with
    // an answer — so a hierarchy refuses such a base rather than picking two of
    // them and producing a surface nobody can explain.
    bool non_manifold = false;

    static LevelConnectivity build(const LevelTopology& topology);

    std::size_t edge_count() const { return edges.size(); }
    std::size_t bytes() const;

    const std::uint32_t* faces_of(std::uint32_t v, std::size_t* count) const {
        return span(vertex_face_offsets, vertex_faces, v, count);
    }
    const std::uint32_t* edges_of(std::uint32_t v, std::size_t* count) const {
        return span(vertex_edge_offsets, vertex_edges, v, count);
    }

   private:
    static const std::uint32_t* span(const std::vector<std::uint32_t>& offsets,
                                     const std::vector<std::uint32_t>& values, std::uint32_t i,
                                     std::size_t* count) {
        const std::uint32_t begin = offsets[i], end = offsets[i + 1];
        *count = end - begin;
        return values.data() + begin;
    }
};

// -- where a child vertex came from -------------------------------------------

enum class SubdivisionOrigin : std::uint8_t {
    VertexPoint = 0,  // the parent vertex, moved by the vertex rule
    EdgePoint = 1,    // the point on a parent edge
    FacePoint = 2,    // the centroid of a parent face
};

// The child index ranges. Everything about ancestry is arithmetic on these,
// which is why no per-vertex parent record exists.
struct ChildLayout {
    std::uint32_t vertex_base = 0;  // always 0
    std::uint32_t edge_base = 0;    // parent vertex count
    std::uint32_t face_base = 0;    // + parent edge count
    std::uint32_t total = 0;

    static ChildLayout of(const LevelTopology& parent, const LevelConnectivity& conn) {
        ChildLayout l;
        l.edge_base = parent.vertex_count;
        l.face_base = l.edge_base + static_cast<std::uint32_t>(conn.edges.size());
        l.total = l.face_base + parent.face_count;
        return l;
    }

    SubdivisionOrigin origin_of(std::uint32_t child) const {
        if (child < edge_base) return SubdivisionOrigin::VertexPoint;
        if (child < face_base) return SubdivisionOrigin::EdgePoint;
        return SubdivisionOrigin::FacePoint;
    }
    // The parent element the child was generated from: a vertex, an edge, or a
    // face, according to `origin_of`.
    std::uint32_t source_of(std::uint32_t child) const {
        if (child < edge_base) return child;
        if (child < face_base) return child - edge_base;
        return child - face_base;
    }
};

// -- the two things this file produces ----------------------------------------

// The level above `parent`. Deterministic in the sense the header opens with:
// the same parent gives the same corner list, byte for byte, on every platform.
LevelTopology subdivide_topology(const LevelTopology& parent, const LevelConnectivity& conn);

// The child level's positions with NO detail applied — the pure subdivision
// surface a detail coefficient is measured against.
//
// `out` is resized to the child vertex count and fully written.
void subdivide_positions(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat3>& parent_positions,
                         std::vector<kernel::cfloat3>* out);

// The same rule over the child vertices in `child_vertices` and NO OTHERS.
// `inout` must already hold a full child array; the entries outside the list
// are not read and not written, which is what makes a dab cost what it touched.
//
// The result is BIT-IDENTICAL to the full call for the vertices it does write —
// same expression, same order of operations — which is the property
// `test_multires_dirty` asserts rather than assumes.
void subdivide_positions_partial(const LevelTopology& parent, const LevelConnectivity& conn,
                                 const std::vector<kernel::cfloat3>& parent_positions,
                                 const std::vector<std::uint32_t>& child_vertices,
                                 std::vector<kernel::cfloat3>* inout);

// The same stencils over an ATTRIBUTE rather than a position. Used for UVs and
// colours over their own connectivity, where a seam is a boundary and the
// boundary rule interpolates along it instead of across it.
void subdivide_attribute(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat2>& in, std::vector<kernel::cfloat2>* out);
void subdivide_attribute(const LevelTopology& parent, const LevelConnectivity& conn,
                         const std::vector<kernel::cfloat3>& in, std::vector<kernel::cfloat3>* out);

// -- local propagation --------------------------------------------------------

// Which child vertices change when these parent vertices move.
//
// THE SUPPORT, EXACTLY, and it is worth writing down because the intuitive
// answer ("the children of these vertices") is wrong in a way that leaves
// visible seams: a vertex point reads the face points of every incident face,
// and a face point reads every corner of its face, so moving one vertex reaches
// the vertex points of every vertex sharing a FACE with it — including the one
// across the diagonal, which shares no edge. So the rule is:
//
//     for every face incident to a dirty parent vertex,
//         its corners' vertex points, its edges' edge points, its face point
//
// Appended to `out` sorted and free of duplicates, so a caller can compare two
// runs byte for byte.
void dirty_children(const LevelTopology& parent, const LevelConnectivity& conn,
                    const std::vector<std::uint32_t>& dirty_parents,
                    std::vector<std::uint32_t>* out);

// -- crossing to and from the interchange mesh --------------------------------

// The level-0 cage of `m`: its quads when it has them, its triangles when it
// does not, expressed over `class_of` — the weld classes an `Adjacency` built.
//
// `class_of` has one entry per raw vertex of `m` and `class_count` is how many
// distinct classes there are; both come straight from `Adjacency`. Returns
// false, leaving `out` untouched, when the cage cannot be expressed: an empty
// mesh, an index past the end, or a face whose corners collapse to fewer than
// three distinct classes.
bool base_topology_from_mesh(const Mesh& m, const std::uint32_t* class_of,
                             std::uint32_t class_count, LevelTopology* out);

// The level's FACES into a mesh, without touching its vertex arrays: `quads`
// filled when the level is all quads, `indices` their triangulation. Separate
// from the call below because a cached level mesh already holds its positions
// and normals and must not have them cleared to get an index buffer.
void level_faces_into(const LevelTopology& topology, Mesh* out);

// The level as a flat `mesh::Mesh`: `quads` filled when the level is all quads,
// `indices` their triangulation in the order `mesh_data.h`'s invariant
// requires — quad (a,b,c,d) is triangles (a,b,c) and (a,c,d).
//
// Positions are taken as given; attributes are the caller's business, because
// they live on a different connectivity.
void level_to_mesh(const LevelTopology& topology, const std::vector<kernel::cfloat3>& positions,
                   Mesh* out);

}  // namespace mesh
}  // namespace clay
