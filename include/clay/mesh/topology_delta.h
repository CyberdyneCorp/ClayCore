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
    std::size_t halfedge_count() const { return halfedges_.size(); }
    std::size_t edge_count() const { return edges_.size(); }
    std::size_t face_count() const { return faces_.size(); }

    // The entries themselves, READ-ONLY. For what has to follow a replay
    // without being part of the surface -- the sculptor's chunked index keeps
    // itself in step from the face and vertex entries alone -- and for a test
    // asserting that a record's `after` end is the live surface.
    const std::vector<ElementDelta<DynamicVertex, VertexId>>& vertex_entries() const {
        return vertices_;
    }
    const std::vector<ElementDelta<DynamicHalfEdge, HalfEdgeId>>& halfedge_entries() const {
        return halfedges_;
    }
    const std::vector<ElementDelta<DynamicEdge, EdgeId>>& edge_entries() const { return edges_; }
    const std::vector<ElementDelta<DynamicFace, FaceId>>& face_entries() const { return faces_; }

    void clear();

    // What this record OWNS, for a memory budget. Not `sizeof`: the arrays are
    // the payload, and a record following one dab costs nothing like one
    // following a stroke. Capacities included, so it depends on the allocator's
    // growth policy and is right for a BUDGET, wrong for an exact assertion.
    std::size_t bytes() const;

    // Exactly `encode().size()`, without encoding: a 24-byte header, then 122 /
    // 114 / 42 / 66 bytes per vertex / half-edge / edge / face entry. Fixed-width,
    // so it is the same on every platform and is the number to assert a count
    // against.
    std::size_t encoded_size() const;

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

// -- a gesture a host can replay -----------------------------------------------
//
// (undo-a-dynamic-stroke-across-the-abi.) A `TopologyDelta` plus the two
// `SurfaceMark`s that say which state it was taken from and which it left.
//
// WHY THE MARKS. `TopologyDelta::revert` trusts its caller: it writes whatever
// it holds into whatever surface it is given, and returns true. Replayed out of
// order that corrupts the surface -- and not only when the two gestures
// overlap in space. A later stroke on the far side of a sphere reuses the slots
// an earlier one freed, so reverting the earlier one under it rewrites elements
// the later one owns. Measured: 27 vertex, 161 half-edge, 80 edge and 54 face
// slots shared by two strokes on opposite hemispheres, and `validate` failing
// after the out-of-order revert.
//
// A content comparison (each recorded element against the record's end state)
// catches every overlap it can see, in 0.051 ms over 13,759 elements, and was
// rejected: an UNRECORDED edit in between can rewrite an element the record
// does not name while a named one still points at it, and the check then
// passes on a replay that is not sound. The mark is exact for the contract
// actually offered -- last in, first out, nothing unrecorded in between -- and
// costs two integer compares.

enum class ReplayDirection { Revert, Apply };

// What a replay did.
enum class ReplayResult {
    Applied,
    // The surface was already at the target: nothing written, no revision
    // advanced. Reverting twice is reverting once.
    NoOp,
    // The surface is at neither end of the record. Nothing written.
    Mismatch,
};

// What decoding refused, when it did.
enum class GestureDecode { Ok, Malformed, ForwardVersion };

class RecordedGesture {
   public:
    const TopologyDelta& delta() const { return delta_; }
    // For the sculptor's capture only. Writing entries here without moving the
    // marks makes a record that describes a state no surface was ever in.
    TopologyDelta& delta_mutable() { return delta_; }

    SurfaceMark before() const { return before_; }
    SurfaceMark after() const { return after_; }
    bool empty() const { return delta_.empty(); }

    // CAPTURE. An empty record binds to whatever state the surface is in; a
    // non-empty one accepts a further stamp only when the surface is still
    // where the record left it. Anything else would coalesce two unrelated
    // histories -- a record from another surface, or one with an unrecorded
    // stamp or a replay in between -- into one step.
    bool can_capture_on(const DynamicSurface& surface) const;
    void begin_capture(const DynamicSurface& surface);
    void end_capture(const DynamicSurface& surface);

    // The replay guard, read-only. `Applied` here means "may proceed".
    ReplayResult guard(const DynamicSurface& surface, ReplayDirection direction) const;

    // Empties and unbinds. KEEPS the capacity, so a host reusing one record per
    // stroke allocates on the first stroke only.
    void clear();

    // Resident: the delta's `bytes()` plus this wrapper. Allocator-dependent.
    std::size_t bytes() const { return sizeof(*this) - sizeof(TopologyDelta) + delta_.bytes(); }
    // Exact: `kHeaderBytes + delta().encoded_size()`, which is
    // 56 + 122V + 114H + 42E + 66F.
    static constexpr std::size_t kHeaderBytes = 32;
    std::size_t encoded_size() const { return kHeaderBytes + delta_.encoded_size(); }

    //   u32 'CDGR'  u16 version  u16 reserved
    //   u64 lineage  u64 epoch_before  u64 epoch_after
    //   then TopologyDelta::encode(), unchanged.
    //
    // For SPILLING a record out of memory while the surface it was captured on
    // is still alive. Not crash recovery: a surface's lineage is not in its own
    // encoding, so a reloaded surface matches no record.
    std::vector<std::uint8_t> encode() const;
    // Refuses a truncated or hostile buffer before allocating, and reports a
    // wrapper or inner version above this build's as `ForwardVersion`. Leaves
    // `out` untouched unless it returns `Ok`.
    static GestureDecode decode(const std::uint8_t* data, std::size_t size, RecordedGesture* out);

   private:
    TopologyDelta delta_;
    SurfaceMark before_;
    SurfaceMark after_;
};

}  // namespace mesh
}  // namespace clay
