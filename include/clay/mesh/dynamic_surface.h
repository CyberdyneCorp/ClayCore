#pragma once

// A TRIANGULAR SURFACE WHOSE CONNECTIVITY CAN CHANGE (dynamic-topology spec,
// add-dynamic-topology).
//
// THE GAP THIS CLOSES, stated precisely, because "add dyntopo" is not it:
// **there is no representation in this library whose connectivity can change.**
// `Mesh` is flat arrays a mutation renumbers. `Adjacency` is CSR that goes
// stale on a count change and says so. `Bvh::refit` refuses a topology change
// by design. `VertexDeltas` deliberately records no indices. Every one of those
// is a correct decision for the representation it serves, and together they
// mean adaptive topology cannot be retrofitted — it has to be a representation
// of its own. This is that representation.
//
// BESIDE `MeshSculptor`, NEVER INSIDE IT. The fixed-topology contract — no verb
// creates, splits, deletes or reorders a polygon, and `indices` and `quads`
// come out byte-identical — is what makes a mesh layer worth holding after a
// retopology pass, and it is untouched. `mesh::Mesh` remains the interchange
// format and gains nothing; a dynamic surface crosses to it at explicit
// boundaries through `from_mesh` and `to_mesh`.
//
// TRIANGLES, and the export says so. `to_mesh` writes `quads` empty and
// re-derives none — see D11 in the change's design. Re-deriving them would be a
// pairing heuristic, and the quad export is what the retopology pipeline
// downstream consumes; a pipeline fed heuristic quads that vary with the sculpt
// is worse off than one told plainly what this representation is.
//
// ATTRIBUTE DOMAINS ARE SEPARATED FROM THE FIRST COMMIT, even though P0 does
// not author corner UVs. A seam is represented in a flat mesh by
// position-coincident duplicate vertices, which a mutable surface must either
// weld — destroying the UVs — or treat as disconnected geometry, which cracks
// under remeshing. The representation has to be able to say "one geometric
// vertex, two UVs" before any operator runs, because retrofitting an attribute
// domain means rewriting every operator that interpolates.

#include <cstdint>
#include <optional>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/parallel/cancel.h"
#include "clay/mesh/adjacency.h"  // kDefaultWeldEpsilon
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/slot_pool.h"

namespace clay {
namespace mesh {

// `VertexId`, `HalfEdgeId`, `EdgeId` and `FaceId` are in `slot_pool.h`, with
// the storage they address — a handle is not the surface, and `WorkItemId` is
// built out of a `VertexId` without wanting anything else in this file.

// What an operator may not do to an edge. Flags ON THE EDGE rather than policy
// in the remesher: an operator refuses on its own rather than depending on a
// caller to have filtered its input, and an operator that is safe only when
// called correctly is a bug waiting for the second caller.
enum class EdgeConstraint : std::uint32_t {
    None = 0,
    // An open border: the edge has one incident face. Collapsing across one can
    // close a boundary loop, which is the surface quietly changing genus.
    Boundary = 1u << 0,
    // Two corners of this edge carry different UVs. Welding across it destroys
    // the seam silently, which is the failure the corner domain exists for.
    UvSeam = 1u << 1,
    // A crease the artist or the importer marked. Resists flipping and
    // smoothing; still splits, because splitting a crease keeps it a crease.
    Sharp = 1u << 2,
    // A material boundary, which a downstream bake reads.
    Material = 1u << 3,
    // The caller said no. Honoured above every heuristic.
    UserLocked = 1u << 4,
};

inline std::uint32_t operator|(EdgeConstraint a, EdgeConstraint b) {
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}
inline std::uint32_t operator|(std::uint32_t a, EdgeConstraint b) {
    return a | static_cast<std::uint32_t>(b);
}
inline bool has_constraint(std::uint32_t flags, EdgeConstraint c) {
    return (flags & static_cast<std::uint32_t>(c)) != 0;
}
inline std::uint32_t& operator|=(std::uint32_t& a, EdgeConstraint b) {
    a |= static_cast<std::uint32_t>(b);
    return a;
}

// -- the records --------------------------------------------------------------

struct DynamicVertex {
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    // GEOMETRIC and derived, never authored: recomputed locally after anything
    // moves, for the reason `class_normal` already gives — displacement is
    // about where the surface is, not how it shades.
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);
    // Vertex-domain attributes. Colour is the model's; mask is the painted
    // freeze, carried per vertex so a converted surface does not lose it.
    kernel::cfloat3 color = kernel::cf3(1, 1, 1);
    float mask = 0.0f;
    // ONE outgoing half-edge, any of them. The rest of the ring is reachable
    // from it, so this is a seed rather than a list — a list would have to be
    // maintained by every operator and would be the thing they get wrong.
    HalfEdgeId outgoing;
    std::uint32_t flags = 0;
};

struct DynamicHalfEdge {
    VertexId origin;
    FaceId face;      // invalid on a boundary half-edge
    HalfEdgeId next;  // around the face, counter-clockwise
    HalfEdgeId twin;  // the opposite half-edge; always present
    EdgeId edge;
    // CORNER-DOMAIN attributes live here, because a half-edge IS a face corner.
    // This is the whole of D3: two half-edges leaving the same geometric vertex
    // on opposite sides of a seam carry different UVs, and the vertex stays one
    // vertex.
    kernel::cfloat2 uv = kernel::cf2(0, 0);
};

struct DynamicEdge {
    HalfEdgeId halfedge;  // one of the two; the other is its twin
    std::uint32_t constraints = 0;
};

struct DynamicFace {
    HalfEdgeId halfedge;  // one of the three
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);
    std::uint32_t flags = 0;
};

// What `from_mesh` refused, when it did.
enum class DynamicBuildError {
    None = 0,
    EmptyMesh,
    IndexOutOfRange,
    DegenerateTriangle,
    // Three or more faces on one edge. A half-edge surface cannot express it,
    // and silently dropping the third face would be a conversion that changes
    // the model without saying so.
    NonManifoldEdge,
};

struct DynamicSurfaceBuildOptions {
    // Vertices closer than this are one geometric vertex. The same rule
    // `Adjacency` uses, and the same default, so a mesh welds the same way on
    // both paths.
    float weld_epsilon = kDefaultWeldEpsilon;
    // Mark an edge as a UV seam when the two corners meeting across it disagree
    // by more than this. Zero means exact.
    float uv_seam_epsilon = 1e-6f;
    // Refuse rather than repair. A conversion that quietly drops a face is a
    // conversion the caller cannot reason about.
    bool refuse_non_manifold = true;
};

struct DynamicSurfaceExportOptions {
    // Emit vertex normals. A surface imported from a mesh with none still
    // exports none, so a layer's exported attribute set does not change under
    // a round trip.
    bool normals = true;
    bool colors = true;
    bool uvs = true;
};

// The counts a caller reports or asserts on.
struct DynamicSurfaceStats {
    std::size_t vertices = 0;
    std::size_t edges = 0;
    std::size_t halfedges = 0;
    std::size_t faces = 0;
    std::size_t boundary_edges = 0;
    // Slots allocated but not live, so a caller can see the cost of never
    // compacting and decide whether to.
    std::size_t dead_slots = 0;
};

// -- the surface --------------------------------------------------------------

class DynamicSurface {
   public:
    DynamicSurface() = default;

    // Build from a flat mesh. Returns nullopt and sets `out_error` when the
    // input cannot be expressed — see `DynamicBuildError`.
    // CANCELLABLE, and BUILD-THEN-PUBLISH: the surface is assembled into a
    // local and handed back only on success, so a cancelled conversion leaves
    // the caller's world exactly as it found it rather than half a surface.
    // That is not a nicety — a half-built half-edge structure is one that
    // validates in some places and crashes a walk in others.
    static std::optional<DynamicSurface> from_mesh(
        const Mesh& mesh, const DynamicSurfaceBuildOptions& options = {},
        DynamicBuildError* out_error = nullptr, const parallel::CancelToken* cancel = nullptr);

    // Export to a flat mesh. Splits a geometric vertex into as many export
    // vertices as it has distinct corner attributes, so a seam survives the
    // round trip as the duplicates a flat mesh represents it with.
    // Cancellable for the same reason. A cancelled export returns an EMPTY
    // mesh rather than a partial one: a caller that ignored the cancel and drew
    // the result would draw a fraction of the model, which is worse than
    // drawing nothing.
    Mesh to_mesh(const DynamicSurfaceExportOptions& options = {},
                 const parallel::CancelToken* cancel = nullptr) const;

    // -- element access ------------------------------------------------------
    const DynamicVertex* vertex(VertexId id) const { return vertices_.get(id); }
    DynamicVertex* vertex(VertexId id) { return vertices_.get(id); }
    const DynamicHalfEdge* halfedge(HalfEdgeId id) const { return halfedges_.get(id); }
    DynamicHalfEdge* halfedge(HalfEdgeId id) { return halfedges_.get(id); }
    const DynamicEdge* edge(EdgeId id) const { return edges_.get(id); }
    DynamicEdge* edge(EdgeId id) { return edges_.get(id); }
    const DynamicFace* face(FaceId id) const { return faces_.get(id); }
    DynamicFace* face(FaceId id) { return faces_.get(id); }

    bool live(VertexId id) const { return vertices_.live(id); }
    bool live(HalfEdgeId id) const { return halfedges_.live(id); }
    bool live(EdgeId id) const { return edges_.live(id); }
    bool live(FaceId id) const { return faces_.live(id); }

    const SlotPool<DynamicVertex, VertexId>& vertices() const { return vertices_; }
    const SlotPool<DynamicHalfEdge, HalfEdgeId>& halfedges() const { return halfedges_; }
    const SlotPool<DynamicEdge, EdgeId>& edges() const { return edges_; }
    const SlotPool<DynamicFace, FaceId>& faces() const { return faces_; }

    DynamicSurfaceStats stats() const;
    std::size_t bytes() const;

    // -- serialization --------------------------------------------------------
    //
    // A VERSIONED FORMAT OF ITS OWN, not an overload of the mesh blob. A flat
    // mesh's encoding cannot express a half-edge structure, and widening it to
    // try would make every existing reader's understanding of that chunk wrong.
    //
    // GENERATIONS ARE PRESERVED, and that is worth stating because the cheap
    // answer is not to. A handle is a slot AND a generation; a document reloaded
    // with the generations reset would hand back handles that a saved undo
    // record, a saved selection or a host's own bookkeeping would silently
    // mis-resolve — the exact failure the generation exists to prevent, arriving
    // through the one path nobody tests interactively.
    //
    // DEAD SLOTS ARE NOT preserved. They carry nothing, and a file that stored
    // them would grow with a session's history rather than with its content.
    // The generation of a live slot survives; the free list is rebuilt.
    std::vector<std::uint8_t> encode() const;
    // Refuses a truncated, hostile or newer buffer rather than returning a
    // surface whose connectivity points at nothing. Returns false and leaves
    // `out` untouched.
    static bool decode(const std::uint8_t* data, std::size_t size, DynamicSurface* out);

    // -- traversal -----------------------------------------------------------
    //
    // The queries every operator is written in terms of. Each is a few lines,
    // and each is here rather than in the operators because an operator that
    // walks the connectivity by hand is an operator that walks it slightly
    // differently from the last one.

    VertexId origin_of(HalfEdgeId h) const;
    // The vertex a half-edge points AT: its twin's origin.
    VertexId target_of(HalfEdgeId h) const;
    HalfEdgeId twin_of(HalfEdgeId h) const;
    HalfEdgeId next_of(HalfEdgeId h) const;
    // The remaining corner of a triangle: next of next.
    HalfEdgeId prev_of(HalfEdgeId h) const;
    EdgeId edge_of(HalfEdgeId h) const;
    FaceId face_of(HalfEdgeId h) const;
    // One of the edge's two half-edges, and the other.
    HalfEdgeId halfedge_of(EdgeId e) const;

    bool is_boundary_halfedge(HalfEdgeId h) const;
    bool is_boundary_edge(EdgeId e) const;
    bool is_boundary_vertex(VertexId v) const;

    kernel::cfloat3 position_of(VertexId v) const;
    float edge_length(EdgeId e) const;
    kernel::cfloat3 edge_midpoint(EdgeId e) const;

    // Every half-edge leaving `v`, in ring order, appended to `out`. Cleared
    // first. Returns false when the ring does not close, which the validator
    // treats as corruption rather than as a boundary.
    bool outgoing_halfedges(VertexId v, std::vector<HalfEdgeId>* out) const;
    // The vertices one edge away, appended to `out`. Cleared first.
    bool one_ring(VertexId v, std::vector<VertexId>* out) const;
    // The faces incident to `v`. Cleared first.
    bool incident_faces(VertexId v, std::vector<FaceId>* out) const;
    // The three vertices of a face, in winding order.
    bool face_vertices(FaceId f, VertexId out[3]) const;
    // The valence of `v`, or 0 if its ring does not close.
    std::size_t valence(VertexId v) const;

    // -- geometry ------------------------------------------------------------
    kernel::cfloat3 face_normal(FaceId f) const;
    // Twice the triangle's area, which is what an area weight wants and what a
    // degeneracy test compares against zero.
    float face_area_x2(FaceId f) const;
    // Angle-weighted over the incident faces, for the reason `class_normal`
    // gives: an area-weighted normal on a lattice-derived mesh tracks the
    // lattice rather than the surface, and `inflate` turns that into a
    // golf-ball dimple.
    kernel::cfloat3 compute_vertex_normal(VertexId v) const;

    // Recompute the normals of these faces and of every vertex they touch.
    // The unit of a local update: an operator names what it changed, and this
    // is what makes the change visible.
    void refresh_normals(const std::vector<FaceId>& faces);
    void refresh_all_normals();

    // -- revisions -----------------------------------------------------------
    //
    // TRIPLE, not single. A host that must re-upload an index buffer only when
    // connectivity changed needs to be told which of the three happened, and
    // the same distinction serves cache invalidation for anything derived.
    std::uint64_t topology_revision() const { return topology_revision_; }
    std::uint64_t geometry_revision() const { return geometry_revision_; }
    std::uint64_t attribute_revision() const { return attribute_revision_; }

    void bump_topology() { ++topology_revision_; }
    void bump_geometry() { ++geometry_revision_; }
    void bump_attributes() { ++attribute_revision_; }

    // -- mutation, for the operators -----------------------------------------
    //
    // Public because `topology_ops.cpp` is a separate translation unit and
    // these are its vocabulary; NOT because a caller should reach for them.
    // Every one of them leaves the surface temporarily inconsistent by design —
    // that is what a topology operator is — and only the operators know how to
    // put it back. The validator is what proves they did.

    VertexId create_vertex(const DynamicVertex& v);
    HalfEdgeId create_halfedge(const DynamicHalfEdge& h);
    EdgeId create_edge(const DynamicEdge& e);
    FaceId create_face(const DynamicFace& f);

    bool erase_vertex(VertexId id);
    bool erase_halfedge(HalfEdgeId id);
    bool erase_edge(EdgeId id);
    bool erase_face(FaceId id);

    // Wire a face's three half-edges into a closed loop, set their face, and
    // point the face at the first. The one operation every operator ends with,
    // written once so they cannot each get the winding subtly different.
    void bind_face(FaceId f, HalfEdgeId a, HalfEdgeId b, HalfEdgeId c);
    // Pair two half-edges as twins and hang them off one edge.
    void bind_edge(EdgeId e, HalfEdgeId a, HalfEdgeId b);
    // Point a vertex at an outgoing half-edge that is actually live, preferring
    // a boundary one so `is_boundary_vertex` stays cheap.
    void refresh_outgoing(VertexId v);

    // -- decoding ------------------------------------------------------------
    SlotPool<DynamicVertex, VertexId>& vertices_mutable() { return vertices_; }
    SlotPool<DynamicHalfEdge, HalfEdgeId>& halfedges_mutable() { return halfedges_; }
    SlotPool<DynamicEdge, EdgeId>& edges_mutable() { return edges_; }
    SlotPool<DynamicFace, FaceId>& faces_mutable() { return faces_; }

   private:
    SlotPool<DynamicVertex, VertexId> vertices_;
    SlotPool<DynamicHalfEdge, HalfEdgeId> halfedges_;
    SlotPool<DynamicEdge, EdgeId> edges_;
    SlotPool<DynamicFace, FaceId> faces_;

    std::uint64_t topology_revision_ = 1;
    std::uint64_t geometry_revision_ = 1;
    std::uint64_t attribute_revision_ = 1;

    // Whether the source mesh carried these, so an export does not manufacture
    // an attribute the layer never had.
    bool had_normals_ = false;
    bool had_colors_ = false;
    bool had_uvs_ = false;

   public:
    bool had_normals() const { return had_normals_; }
    bool had_colors() const { return had_colors_; }
    bool had_uvs() const { return had_uvs_; }
    void set_source_attributes(bool normals, bool colors, bool uvs) {
        had_normals_ = normals;
        had_colors_ = colors;
        had_uvs_ = uvs;
    }
};

}  // namespace mesh
}  // namespace clay
