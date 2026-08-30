#pragma once

// ONE GESTURE, ONE SPARSE, REVERSIBLE UNDO STEP (dynamic-topology spec,
// add-dynamic-topology).
//
// WHY NOT A SNAPSHOT. A multi-million-triangle snapshot per stroke is not an
// undo system, it is a memory leak with a keyboard shortcut. So the record is
// what CHANGED: the elements created, the elements deleted, and the elements
// whose contents were rewritten, each with the state it had before and the
// state it ended with.
//
// COALESCED PER GESTURE, exactly as `VertexDeltas` already coalesces positions.
// An element touched by forty stamps of one stroke appears ONCE, keeping the
// FIRST `before` and the LAST `after`. The record's size is therefore bounded
// by the elements the gesture REACHED, not by the stamps it took — which is the
// difference between an undo step and a memory leak on a stroke that goes back
// over its own path.
//
// SLOTS AND GENERATIONS ARE BOTH RESTORED. Reverting a creation does not merely
// delete an element: it puts the slot back in the state it was in, generation
// included, so a handle taken before the gesture still resolves afterwards. An
// undo that left the geometry right and the handles stale would be an undo the
// spatial index and the host's upload buffers could not survive.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "clay/mesh/dynamic_surface.h"

namespace clay {
namespace mesh {

// One element's history over a gesture. `before` is the state the gesture found
// and `after` the state it left; the two flags say whether the element existed
// at either end, which is what distinguishes a creation from a deletion from a
// rewrite.
template <typename T, typename Id>
struct ElementDelta {
    // TWO HANDLES, not one, and the reason is the generation. An element the
    // gesture DELETED had its generation bumped by the erase, and one the
    // gesture CREATED never had a "before" generation at all. Reverting has to
    // put back the handle that existed before, and re-applying has to put back
    // the one that existed after; a single field cannot be both, and deriving
    // one from the other by adding or subtracting one is arithmetic on a
    // counter whose steps are not the caller's to predict.
    Id before_id;
    Id after_id;
    T before{};
    T after{};
    bool existed_before = false;
    bool exists_after = false;
};

class TopologyDelta {
   public:
    // Record the state of an element BEFORE it is touched. Idempotent per
    // element: the first call wins, which is what makes the record coalesced.
    // Called by every operator before it writes.
    void note_vertex(const DynamicSurface& s, VertexId id);
    void note_halfedge(const DynamicSurface& s, HalfEdgeId id);
    void note_edge(const DynamicSurface& s, EdgeId id);
    void note_face(const DynamicSurface& s, FaceId id);

    // Record an element the operator has just CREATED.
    //
    // A separate call rather than a flag on `note`, because `note` reads
    // liveness from the pool and a freshly created element is live — so noting
    // one the ordinary way records it as having existed before the gesture, and
    // the revert then leaves it behind. That was the first defect these tests
    // found, and it is invisible without a fingerprint over the slots.
    void note_new_vertex(VertexId id);
    void note_new_halfedge(HalfEdgeId id);
    void note_new_edge(EdgeId id);
    void note_new_face(FaceId id);

    // Record the state an element ended in. Called after an operator finishes,
    // and re-callable: the last call wins.
    void sync_vertex(const DynamicSurface& s, VertexId id);
    void sync_halfedge(const DynamicSurface& s, HalfEdgeId id);
    void sync_edge(const DynamicSurface& s, EdgeId id);
    void sync_face(const DynamicSurface& s, FaceId id);

    // Put the surface back exactly as the gesture found it, and put it back the
    // way the gesture left it. Both are idempotent, and revert-then-apply
    // returns the surface exactly.
    bool revert(DynamicSurface& surface) const;
    bool apply(DynamicSurface& surface) const;

    bool empty() const {
        return vertices_.empty() && halfedges_.empty() && edges_.empty() && faces_.empty();
    }
    std::size_t element_count() const {
        return vertices_.size() + halfedges_.size() + edges_.size() + faces_.size();
    }
    std::size_t vertex_count() const { return vertices_.size(); }
    std::size_t face_count() const { return faces_.size(); }

    void clear();

    // What this record OWNS, for a memory budget. Not `sizeof`: the arrays are
    // the payload, and a record following one dab costs nothing like one
    // following a stroke.
    std::size_t bytes() const;

    // -- encoding -------------------------------------------------------------
    //
    // Fixed-width and positional, like `VertexDeltas`: a crash artifact paired
    // with one document, cheap to write on every step rather than forgiving to
    // read years later. The version is there so a build that does not
    // understand it REFUSES — a recovery that silently drops what it could not
    // read is the failure the feature exists to prevent.
    std::vector<std::uint8_t> encode() const;
    // Refuses a truncated or inconsistent buffer rather than returning a record
    // that reverts a surface to garbage. Returns false and leaves `out`
    // untouched.
    static bool decode(const std::uint8_t* data, std::size_t size, TopologyDelta* out);

   private:
    std::vector<ElementDelta<DynamicVertex, VertexId>> vertices_;
    std::vector<ElementDelta<DynamicHalfEdge, HalfEdgeId>> halfedges_;
    std::vector<ElementDelta<DynamicEdge, EdgeId>> edges_;
    std::vector<ElementDelta<DynamicFace, FaceId>> faces_;

    // slot -> index, per kind. Keyed by SLOT rather than by the whole handle:
    // an operator that deletes a slot and another that later reuses it are the
    // same slot's history, and coalescing them is what keeps one entry per
    // element rather than one per generation.
    std::unordered_map<std::uint32_t, std::uint32_t> vertex_slot_, halfedge_slot_, edge_slot_,
        face_slot_;
};

}  // namespace mesh
}  // namespace clay
