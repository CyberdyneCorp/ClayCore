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
// WHAT "LOCAL" MEANS HERE, precisely, because it is the requirement: moving a
// vertex refits its leaf and the leaf's ANCESTORS, which is logarithmic in the
// number of leaves — not the whole tree, and not the whole surface. A split or
// a collapse touches the leaves whose faces changed and no others, and the
// test asserts that rather than inferring it from a timing.

#include <cstdint>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/dynamic_surface.h"

namespace clay {
namespace mesh {

struct DynamicBvhOptions {
    // Faces per leaf. A few hundred is the band where the leaf is big enough to
    // be worth uploading as a unit and small enough that a brush touching one
    // does not drag in a neighbourhood it does not need.
    std::size_t target_leaf_faces = 256;
    // Above this a leaf splits; below it, two siblings merge. The gap is
    // hysteresis: a leaf hovering at the threshold must not split and merge on
    // alternate stamps.
    std::size_t max_leaf_faces = 512;
    std::size_t min_leaf_faces = 64;
};

// One chunk. `revision` advances whenever anything in it changes, so a host can
// ask "what do I need to re-upload" without diffing geometry.
struct SurfaceLeaf {
    math::Aabb bounds;
    std::vector<FaceId> faces;
    std::uint64_t revision = 0;
    // Set when the faces moved but the membership did not, and when the
    // membership itself changed. A host re-uploads an index buffer only for the
    // second.
    bool geometry_dirty = false;
    bool topology_dirty = false;
    // The tree node that owns this leaf.
    std::uint32_t node = 0xffffffffu;
    bool live = false;
};

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
    const std::vector<std::uint32_t>& dirty_leaves() const { return dirty_; }
    void clear_dirty();

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
    std::vector<SurfaceLeaf> leaves_;
    std::vector<Node> nodes_;
    std::uint32_t root_ = 0xffffffffu;
    // face slot -> leaf index. A vector rather than a map: the slot space is
    // dense enough that the vector is smaller and the lookup is a load.
    std::vector<std::uint32_t> face_leaf_;
    std::vector<std::uint32_t> dirty_;
    std::vector<std::uint32_t> dirty_epoch_;
    // `update_many`'s per-stamp leaf list, kept so a stroke of similar stamps
    // allocates on its first one and never again.
    std::vector<std::uint32_t> update_scratch_;
    std::uint32_t epoch_ = 1;
    std::uint64_t revision_ = 1;
    mutable bool tree_stale_ = false;
};

}  // namespace mesh
}  // namespace clay
