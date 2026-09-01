#pragma once

// A SPATIAL INDEX THAT SURVIVES TOPOLOGY CHANGES (dynamic-topology spec,
// add-dynamic-topology).
//
// `mesh::Bvh` cannot do this and is right not to: it is built over a fixed
// triangle list, `refit` refuses a topology change by design, and every one of
// its guarantees depends on the numbering staying put. A surface whose faces
// come and go needs a different structure.
//
// CHUNKED LEAVES, NOT A PER-FACE MUTABLE TREE. Both work; chunks of a few
// hundred faces win because the chunk is simultaneously
//
//   - the BVH leaf,
//   - the brush's candidate set,
//   - the parallel work unit,
//   - the normal-recompute unit,
//   - the dirty-tracking unit, and
//   - the host's upload unit.
//
// A per-face tree gives the first of those and leaves the library to invent a
// different granularity for each of the others. The brick cache already
// demonstrates the shape: a sparse set of fixed units with revisions, refilled
// and uploaded independently.
//
// THE CHUNK ITSELF IS NO LONGER THIS FILE'S (add-extreme-poly-runtime). It is
// `mesh::ChunkTable`, which the fixed sculptor and the hierarchy share: the
// six roles above are claimed for all three representations rather than for
// this one, and what stays here is the PARTITIONER — which faces go in which
// chunk, and the tree over the chunks. `SurfaceLeaf` is now a name for
// `SurfaceChunk`, so every existing reader of a leaf's faces, bounds, revision
// and dirty flags is unchanged; what it gained is three more revisions, an
// arena instead of a vector per leaf, and an acknowledgement.
//
// WHAT "LOCAL" MEANS HERE, precisely, because it is the requirement: moving a
// vertex refits its leaf and the leaf's ANCESTORS, which is logarithmic in the
// number of leaves — not the whole tree, and not the whole surface. A split or
// a collapse touches the leaves whose faces changed and no others, and the
// test asserts that rather than inferring it from a timing.

#include <cstdint>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/surface_chunks.h"

namespace clay {
namespace mesh {

struct DynamicBvhOptions {
    // Faces per leaf. MEASURED, and the same number `ChunkOptions` carries —
    // one size for the library, not one per representation, because a
    // per-representation size is the second granularity the shared table exists
    // to prevent, arriving by the back door. `surface_chunks.h` holds the
    // matrix and `benchmarks/bench_surface_chunks.cpp` produces it; the short
    // version is that 128 minimises the query-plus-normals-plus-index P95 at
    // the 20k footprint and sits at the minimum of the split-and-merge cost,
    // and that 256 — what this field said before anybody measured it here — is
    // beaten by 5 to 6% on the first and by 1.51x against 1.64x on false
    // positives.
    std::size_t target_leaf_faces = 128;
    // Above this a leaf splits; below it, two siblings merge. The gap is
    // hysteresis: a leaf hovering at the threshold must not split and merge on
    // alternate stamps.
    std::size_t max_leaf_faces = 256;
    std::size_t min_leaf_faces = 32;
};

// `SurfaceLeaf` — the chunk record, its four revisions and its face span — is
// in `surface_chunks.h`. It is not redefined here, because two definitions of
// "one chunk" is the exact failure the shared table exists to remove.

class DynamicBvh {
   public:
    static constexpr std::uint32_t kNoLeaf = 0xffffffffu;

    DynamicBvh() = default;

    // Build over every live face of `surface`. Faces are chunked in SLOT ORDER
    // within a spatial sort, so the same surface produces the same partition on
    // every run and every platform.
    void build(const DynamicSurface& surface, const DynamicBvhOptions& options = {});

    // -- incremental maintenance ---------------------------------------------
    //
    // The three an operator calls. Each touches the leaf it names and that
    // leaf's ancestors, and nothing else.
    void insert(const DynamicSurface& surface, FaceId face);
    void erase(FaceId face);
    // The face is still in its leaf; its geometry moved. Refits the leaf and
    // its ancestors.
    void update(const DynamicSurface& surface, FaceId face);
    // Several at once, so one refit pass covers a stamp rather than one per
    // face.
    void update_many(const DynamicSurface& surface, const std::vector<FaceId>& faces);

    // -- queries -------------------------------------------------------------
    //
    // Each is checked against a brute-force oracle in the tests: an index that
    // is fast and wrong is worse than no index.

    // Every face whose triangle reaches the ball. Over-admits at the leaf level
    // and tests exactly at the face level, so the result is exact.
    void faces_in_ball(const DynamicSurface& surface, kernel::cfloat3 centre, float radius,
                       std::vector<FaceId>* out) const;

    struct ClosestPoint {
        bool found = false;
        FaceId face;
        kernel::cfloat3 position = kernel::cf3(0, 0, 0);
        float distance = 0.0f;
    };
    ClosestPoint closest(const DynamicSurface& surface, kernel::cfloat3 p) const;

    struct RayHit {
        bool hit = false;
        FaceId face;
        float t = 0.0f;
        kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    };
    RayHit raycast(const DynamicSurface& surface, kernel::cfloat3 origin,
                   kernel::cfloat3 direction) const;

    // -- dirty tracking -------------------------------------------------------
    //
    // BY EPOCH MARK, not by a hash set per dab. A leaf carries the epoch it was
    // last marked in; the "clear" is an increment of the current epoch, which
    // costs nothing and cannot grow.
    const std::vector<std::uint32_t>& dirty_leaves() const { return table_.dirty(); }
    void clear_dirty();

    // The table itself, for the transport and the ledger. A caller that wants
    // the four revisions, the chunk-local vertex map or the per-chunk
    // acknowledgement asks it directly; `dirty_leaves` and `leaf` stay as the
    // shipped shorthand over the same records.
    const ChunkTable& chunks() const { return table_; }
    ChunkTable& chunks_mutable() { return table_; }

    // -- introspection --------------------------------------------------------
    std::size_t leaf_count() const;
    const SurfaceLeaf* leaf(std::uint32_t index) const;
    std::size_t face_count() const { return face_leaf_.size(); }
    std::uint32_t leaf_of(FaceId face) const;
    math::Aabb bounds() const;

    // The average leaf overlap, as a proxy for how much the partition has
    // decayed under local edits. A refit stays CORRECT and does not stay FAST —
    // the fixed BVH already records that finding — so a caller watches this and
    // rebuilds BETWEEN strokes, never mid-drag.
    float quality() const;
    // Whether the caller should rebuild. Advisory: nothing here rebuilds on its
    // own behalf, for the reason `Bvh::quality` gives — a rebuild produced a
    // better tree in one of five measured deformations and a dramatically worse
    // one in two.
    bool wants_rebuild() const;

    std::size_t bytes() const;

   private:
    struct Node {
        math::Aabb bounds;
        std::uint32_t left = 0xffffffffu;
        std::uint32_t right = 0xffffffffu;
        std::uint32_t parent = 0xffffffffu;
        std::uint32_t leaf = kNoLeaf;
    };

    void rebuild_tree();
    // Rebuilt lazily, from the queries: a stamp inserts thousands of faces and
    // needs one rebuild, not thousands.
    void ensure_tree() const;
    std::uint32_t build_node(std::vector<std::uint32_t>& order, std::size_t begin, std::size_t end,
                             std::uint32_t parent);
    void refit_leaf(const DynamicSurface& surface, std::uint32_t leaf_index);
    void refit_ancestors(std::uint32_t node);
    void mark_dirty(std::uint32_t leaf_index, bool topology);
    std::uint32_t choose_leaf(kernel::cfloat3 centroid) const;
    void split_leaf(const DynamicSurface& surface, std::uint32_t leaf_index);
    math::Aabb face_bounds(const DynamicSurface& surface, FaceId f) const;

    DynamicBvhOptions options_;
    // The chunks, their faces and their dirty set. Shared with the other two
    // representations; what is private to this file is the tree below and the
    // face-to-chunk map beside it.
    ChunkTable table_;
    std::vector<Node> nodes_;
    std::uint32_t root_ = 0xffffffffu;
    // face slot -> leaf index. A vector rather than a map: the slot space is
    // dense enough that the vector is smaller and the lookup is a load.
    std::vector<std::uint32_t> face_leaf_;
    // Scratch for the two operations that run per stamp: the leaves a refit
    // touched, and the half of a chunk a split moves. Members rather than
    // locals, for the reason every other per-stamp buffer in this library is
    // one — a stroke must allocate on its first stamp and never again.
    std::vector<std::uint32_t> touched_;
    std::vector<FaceId> moved_faces_;
    mutable bool tree_stale_ = false;
};

}  // namespace mesh
}  // namespace clay
