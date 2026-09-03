#pragma once

// A TOP-LEVEL TREE OVER A CHUNK TABLE'S BOUNDS: "which chunks does this brush
// reach", answered by a descent rather than by a walk over every chunk.
//
// WHY IT IS IN THE LIBRARY. The extreme-poly requirement states the query path
// as "brush volume -> top-level tree -> candidate chunks -> candidate vertices
// -> exact footprint", and it held on the fixed mesh and the adaptive surface
// because both descend a `Bvh`. It did NOT hold on a multires level: nothing
// builds a ray tree for one — `MeshSculptor::surface_index` deliberately never
// does, a build being 689 ms against 1.24 ms saved per stamp — so every
// unseeded stamp fell back to a scan over the level's classes. Measured at
// about 1.2 ns a vertex: 0.49 ms at 100k level vertices against 1.58 ms at 1M
// for the same 1k footprint, which is the model showing through a fixed
// footprint.
//
// A chunk table is the structure that WAS already there. This tree is what
// turns it into a query, and it is much cheaper to build than a ray tree
// because it partitions ~1/128 as many items — a chunk is 128 faces.
//
// PROMOTED FROM `benchmarks/chunk_tree.h`, where it lived because no runtime
// caller had one, and where its header already recorded the reason it has to be
// a tree: both benchmarks first wrote the query as a walk over every chunk, and
// `bench_extreme_poly` then reported the query stage growing with the MODEL at a
// fixed footprint — the exact claim that benchmark exists to test, failed by its
// own harness.
//
// It is a median split over centroids, the same partition rule the chunk
// partitioners themselves use, so the tree's shape is not a third thing to
// reason about.

#include <cstdint>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/surface_chunks.h"

namespace clay {
namespace mesh {

class ChunkTree {
  public:
    // Partition over every live chunk's bounds. O(chunks log chunks), and
    // chunks are faces/128, so this is the cheap end of what a stroke can pay.
    void build(const ChunkTable& table);

    void clear();
    bool empty() const { return nodes_.empty(); }
    std::size_t nodes() const { return nodes_.size(); }
    std::size_t chunks() const { return ids_.size(); }

    // The root's box: everything the table covers, for a caller sizing a query
    // against the surface rather than against a number it invented.
    math::Aabb bounds() const { return nodes_.empty() ? math::Aabb{} : nodes_[0].bounds; }

    // The chunk ids whose bounds meet `box`. Conservative in the usual way: a
    // chunk is admitted when its BOX meets the query, so the caller still tests
    // what it actually wanted.
    void query(const math::Aabb& box, std::vector<std::uint32_t>* out) const;

    // The chunk whose BOX is nearest `p`, and that distance. Returns
    // `ChunkTable::kNoChunk` for an empty tree.
    //
    // A branch-and-bound descent, so its cost follows the tree's depth rather
    // than the chunk count. It exists to give a nearest-VERTEX search a
    // starting radius that is a property of the surface near `p` and not of the
    // model's extent: seeding that search from the root's size makes the first
    // query box grow with the model, which is the one thing the whole path is
    // for.
    std::uint32_t nearest_chunk(kernel::cfloat3 p, float* out_distance) const;

    // Re-read the bounds of the chunks named in `changed` and repair the
    // interior nodes above them, WITHOUT re-partitioning.
    //
    // A stroke moves vertices, and a chunk's bounds are set when the table is
    // partitioned and do not follow them — so a tree left alone goes stale in
    // exactly the region the brush is working in, which is the one region a
    // query must not miss. This is the `Bvh::refit` discipline and it has the
    // same limit: the partition decays under enough movement and is not
    // repaired here. `wants_rebuild` is how that is reported.
    void refit(const ChunkTable& table, const std::uint32_t* changed, std::size_t count);

    // Whether the partition has decayed enough to be worth rebuilding between
    // gestures. Never a reason to rebuild inside a stamp: a refit stays CORRECT
    // and stops being FAST, which is a maintenance item and not a hot-path one.
    bool wants_rebuild() const { return refits_ >= kRefitsBeforeRebuild; }

  private:
    struct Node {
        math::Aabb bounds;
        std::uint32_t begin = 0, end = 0;
        std::uint32_t left = kNoNode, right = kNoNode;
    };
    static constexpr std::uint32_t kNoNode = 0xffffffffu;
    // Chosen to match `DynamicBvh`'s own deferral threshold rather than picked
    // here: two structures that decay the same way should not be tuned apart.
    static constexpr std::size_t kRefitsBeforeRebuild = 64;

    std::uint32_t split(std::size_t begin, std::size_t end);
    void descend(std::uint32_t node, const math::Aabb& box, std::vector<std::uint32_t>* out) const;
    void descend_nearest(std::uint32_t node, kernel::cfloat3 p, float* best,
                         std::uint32_t* best_id) const;
    void refit_node(std::uint32_t node);

    std::vector<math::Aabb> boxes_;
    std::vector<std::uint32_t> ids_;   // slot -> chunk id
    std::vector<std::uint32_t> slot_;  // chunk id -> index into boxes_, or kNoNode
    std::vector<std::uint32_t> order_;
    std::vector<Node> nodes_;
    std::size_t refits_ = 0;
};

}  // namespace mesh
}  // namespace clay
