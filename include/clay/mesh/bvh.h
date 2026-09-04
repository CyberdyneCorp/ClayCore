#pragma once

// A triangle BVH answering the two questions that turn a mesh into a field:
// how far is the surface, and which side of it are we on.
//
// CPU only, and used at bake time rather than per evaluation, so this is an
// ordinary C++ module — it is deliberately NOT part of the kernel dialect.
//
// The sign is the hard half. Ray parity breaks on a single hole; the
// closest-triangle pseudonormal is exact on a clean closed mesh and meaningless
// near an opening, because the nearest triangle to a point inside the model may
// be one facing away when the wall it should have hit is missing. Real assets
// have holes, flipped normals and self-intersections, so a method that is
// correct on clean input and catastrophic on dirty input is the wrong method.
//
// So insideness comes from the GENERALIZED WINDING NUMBER: the sum over
// triangles of the signed solid angle each subtends at the query point, over
// 4*pi. Exactly 1 inside a closed surface and 0 outside it, and — the property
// that matters — continuous on an open one, passing smoothly through 1/2 across
// a hole. Inside means greater than 1/2.
//
// Summed exactly that is linear in the mesh, which a narrow band cannot afford.
// Each node therefore carries the aggregate area-weighted normal and centroid of
// the triangles beneath it, so a node far from the query contributes one dipole
// term instead of being descended. Near the surface the tree is descended and
// the sum is exact, which is where a narrow band spends its samples anyway.

#include <cstdint>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

// What a query actually DID, for a test that means to gate the tree's shape
// rather than the machine it ran on.
//
// "Ten times the triangles must not cost ten times the work" is a claim about
// SUMMARIZATION -- the walk stops at a distant node and answers with one term
// instead of descending it -- and that is a count. Asserted as a wall clock it
// became a claim about the scheduler instead: on `macos-latest` the ratio has
// twice landed just over its bound with the tree unchanged (5.379 and 5.459
// against 5.333), because both sides run in well under a millisecond and one
// preemption inside the coarse side inflates the ratio. Taking the fastest of
// several passes was the first attempt at that and was not enough.
//
// These counters are what the claim was always about, and they are identical
// on every machine.
// COUNTED IN `winding_number` ONLY, deliberately. Summarizing is that walk's
// own trick -- the dipole branch -- and `closest` prunes by box distance, which
// is ordinary BVH pruning and not what this gates. Instrumenting both measured
// 3.8% on a `signed_distance` sweep with the pointer null (12.87 ms against
// 12.39, interleaved), which is a permanent cost on the path picking and
// meshing use, paid to serve a test. Counting the one walk that summarizes
// keeps the gate and gives the cost back.
struct BvhWalkStats {
    std::uint64_t nodes_visited = 0;
    std::uint64_t triangles_tested = 0;
    // Subtrees answered by a single dipole term instead of being descended --
    // the summarization itself. Zero means the tree is being walked to its
    // leaves everywhere, which is the failure the gate exists to catch.
    std::uint64_t summarized = 0;

    std::uint64_t work() const { return nodes_visited + triangles_tested; }
};

class Bvh {
  public:
    // Build over `m`'s triangles. The mesh is not retained: the triangles are
    // copied in, so the caller may free or edit the mesh afterwards.
    static Bvh build(const Mesh& m);

    bool empty() const { return tris_.empty(); }
    std::size_t triangle_count() const { return tris_.size(); }

    // Where the walks report what they did. BORROWED and null by default, and
    // nothing is counted while it is null -- one predictable branch per node,
    // which is what keeps this out of the cost it is used to measure.
    //
    // Accumulates across queries, so a caller measuring a sweep resets it once
    // and reads it at the end rather than per query.
    void set_walk_stats(BvhWalkStats* stats) const { stats_ = stats; }
    BvhWalkStats* walk_stats() const { return stats_; }
    math::Aabb bounds() const;

    // Distance to the nearest point on the surface. Always non-negative.
    float unsigned_distance(kernel::cfloat3 p) const;

    // The generalized winding number: ~1 inside a closed surface, ~0 outside,
    // and continuous across a hole. `beta` is the distance-over-radius ratio at
    // which a node is summarized rather than descended; larger is more accurate
    // and slower. Pass 0 to sum every triangle exactly, which the tests use to
    // check the approximation against.
    float winding_number(kernel::cfloat3 p, float beta = 2.0f) const;

    bool is_inside(kernel::cfloat3 p, float beta = 2.0f) const {
        return winding_number(p, beta) > 0.5f;
    }

    // The signed distance: the unsigned distance, negated inside.
    float signed_distance(kernel::cfloat3 p, float beta = 2.0f) const {
        float d = unsigned_distance(p);
        return is_inside(p, beta) ? -d : d;
    }

    // The nearest point ON the surface, and which triangle carries it.
    //
    // `unsigned_distance` is this query with the answer thrown away, and is
    // implemented in terms of it. What the extra information buys is any
    // ATTRIBUTE transfer — a colour, a uv, a normal — read from the triangle
    // the closest point landed on and interpolated by its barycentrics.
    // `VoxelGrid::rasterize_mesh` is the first caller: it is how an imported
    // model's vertex colours reach a palette.
    struct ClosestPoint {
        bool found = false;
        float distance = 0.0f;
        kernel::cfloat3 point = kernel::cf3(0, 0, 0);
        std::uint32_t triangle = 0;
        // Barycentrics of `point`: it == a*(1-u-v) + b*u + c*v.
        float u = 0.0f, v = 0.0f;
    };
    ClosestPoint closest(kernel::cfloat3 p) const;

    // The nearest triangle a ray meets, and where on it. `triangle` is the
    // index in the SOURCE mesh, not in this tree's own order — the build
    // permutes triangles, so a hit that named its own storage would be useless
    // to the caller.
    //
    // NO BACK-FACE CULLING: a sculptor pulling on the inside of a shell means
    // it, and a hit reported only from outside would make half a model
    // unpickable.
    struct RayHit {
        bool hit = false;
        float t = 0.0f;
        std::uint32_t triangle = 0;
        // Barycentrics of the hit: p == a*(1-u-v) + b*u + c*v.
        float u = 0.0f, v = 0.0f;
    };
    RayHit raycast(const math::Ray& ray, float tmin = 0.0f, float tmax = 1e30f) const;

    // -- as a spatial index over the surface ---------------------------------
    //
    // These exist because the mesh brushes needed a spatial index and this is
    // already one: it is built over the same triangles, and since `refit` it is
    // cheap to keep current under a stroke. A second structure would be a
    // second thing to keep in step with the vertices.
    //
    // Both are EXACT, not approximate. `triangles_in_ball` returns every
    // triangle with a vertex inside the ball, so a caller wanting the vertices
    // in a ball gets all of them: a vertex is a corner of every triangle it
    // belongs to. It may also return triangles that merely reach into the ball
    // without having a vertex there, which a caller filters.
    //
    // THE TREE MUST BE CURRENT. A stale tree answers for where the surface was,
    // and unlike a raycast — where that is a tolerable drift — a region query
    // would silently return the wrong set of vertices. Refit before asking.
    void triangles_in_ball(kernel::cfloat3 centre, float radius,
                           std::vector<std::uint32_t>* out) const;

    // The nearest triangle CORNER to `p`, which is the nearest mesh vertex that
    // any triangle references. `corner` is 0, 1 or 2 into the triangle.
    struct NearestVertex {
        bool found = false;
        std::uint32_t triangle = 0;
        int corner = 0;
        float distance = 0.0f;
    };
    NearestVertex nearest_vertex(kernel::cfloat3 p) const;

    // -- refitting -----------------------------------------------------------
    //
    // A mesh layer's TOPOLOGY is fixed, which is the representation's defining
    // property and the one these two spend. When a brush moves vertices the
    // tree's shape is still a valid partition of the same triangles; only the
    // bounds are stale. Recomputing the bounds of the triangles that moved and
    // of the ancestors above them is proportional to the brush, where a rebuild
    // is proportional to the mesh — 1.3 s on a 2M-vertex model, against 0.25 ms
    // for the stamp that dirtied it.
    //
    // `changed` names triangles by their index in the SOURCE mesh, the same
    // numbering `RayHit::triangle` and `ClosestPoint::triangle` report.
    //
    // Naming a triangle that did not move is merely wasteful. FAILING to name
    // one that did leaves its ancestors too small, and a bound that is too
    // small is a query that misses geometry — so the caller's obligation is to
    // name a superset, never a subset.
    //
    // Returns false, changing nothing, when `m` does not have the triangle
    // count this tree was built over: that is a caller pairing a tree with the
    // wrong mesh, and topology change is the one thing a refit cannot absorb.
    bool refit(const Mesh& m, const std::uint32_t* changed, std::size_t count);
    // Every triangle, for a global deformation where naming the changed ones
    // would name all of them.
    bool refit(const Mesh& m);

    // How far the tree has drifted from one built for the current positions.
    //
    // A refit keeps a tree CORRECT indefinitely and does not keep it FAST: pull
    // a long arm out of a sphere and the boxes along it swell and overlap, so a
    // query descends both children where it used to descend one.
    //
    // This is the surface-area heuristic's own cost estimate, read rather than
    // optimised — the expected number of triangle tests a random ray through
    // the root has to make. LOWER IS BETTER.
    //
    // COMPARE A TREE AGAINST ITSELF, not against another mesh. It is normalised
    // by the root box, which makes it invariant to a uniform scale, but not to
    // shape or to triangle distribution: a flat grid and a sphere of the same
    // triangle count do not score alike, so there is no absolute threshold to
    // publish and this deliberately does not publish one. What it is for is the
    // reading now against the reading when the tree was built — a rise means
    // that tree's queries are getting slower.
    //
    // NaN when the root box has no surface area (every triangle in one plane).
    // Zero is the BEST end of the scale, so returning it for a tree nothing can
    // be measured about would read as "queries got cheaper"; every comparison
    // against NaN is false instead, which is the honest answer.
    //
    // WHAT IT DOES NOT MEAN IS "REBUILD", and that is worth stating because it
    // is the obvious reading. Measured over five deformations of a sphere — a
    // long thin pull, alternating rings pushed apart, a uniform scale, a shear
    // and a partial collapse — a rebuild beat the refit in exactly one, the
    // shear, by four per cent. In two it was far worse: 32.2 against 20.0 for
    // the pull, 778.5 against 98.8 for the interleaved case. Median split
    // partitions on triangle CENTROIDS, and a deformation that makes triangles
    // large or elongated defeats that as thoroughly as it defeats an existing
    // partition — often more, because a refit at least keeps a clustering
    // chosen while the geometry was still compact.
    //
    // So nothing here rebuilds on its own behalf, and nothing here advises it
    // on the strength of this number alone. It says what queries cost. What to
    // do about that is the caller's judgement.
    float quality() const;

    // Every node's box contains the three vertices of every triangle beneath
    // it. The INVARIANT a refit exists to preserve, and the one whose violation
    // is silent: a box left too small does not make a query slow, it makes it
    // wrong, by pruning a subtree that should have been descended.
    //
    // WITHOUT `m` this checks the tree against ITS OWN stored triangles, so it
    // is blind to the caller error that matters most — a refit that named a
    // SUBSET leaves the tree perfectly self-consistent about geometry that has
    // moved, and this returns true throughout. Pass the mesh to close that: it
    // then also requires every stored triangle to match the mesh's current
    // positions, which is the check that catches an under-named refit.
    //
    // For tests and asserts over a real mesh; not something a query path needs.
    bool bounds_contain_their_triangles(const Mesh* m = nullptr) const;

  private:
    struct Tri {
        kernel::cfloat3 a, b, c;
        // Where this triangle came from. Carried INSIDE the triangle rather
        // than in a parallel array because the build reorders `tris_` with
        // nth_element, and a parallel array would have to be permuted in step.
        std::uint32_t source = 0;
    };
    struct Node {
        math::Aabb box;
        // Winding-number summary of everything beneath this node: the sum of
        // the triangles' area-weighted normals, and their area-weighted
        // centroid. One dipole standing in for the lot.
        kernel::cfloat3 normal_sum = kernel::cf3(0, 0, 0);
        kernel::cfloat3 centroid = kernel::cf3(0, 0, 0);
        // The two quantities `centroid` is the quotient of, kept rather than
        // discarded so that a REFIT can combine a parent from its children in
        // constant time. Summarising a parent from its span instead is O(span),
        // which at the root is the whole mesh — that would make a bottom-up
        // refit cost exactly what the rebuild it replaces costs.
        kernel::cfloat3 weighted_centroid = kernel::cf3(0, 0, 0);
        float area = 0.0f;
        float radius = 0.0f;  // of `box` about `centroid`
        std::int32_t first = 0;
        std::int32_t span = 0;    // triangles beneath this node, leaf or not
        std::int32_t count = 0;   // > 0 only for a leaf: triangles held here
        std::int32_t right = -1;  // left child is this + 1
        std::int32_t parent = -1;  // -1 at the root; the refit walks up it
    };

    // The winding walk, with the counting compiled IN or OUT. A template rather
    // than a null check because the checks measured 2.3% of a signed_distance
    // sweep with the pointer null, which is a permanent charge on a query
    // picking and meshing lean on, paid so one test can assert a count.
    // `if constexpr` leaves the uncounted instantiation with no branches at all.
    template <bool kCount>
    float winding_walk(kernel::cfloat3 p, float beta, BvhWalkStats* stats) const;

    std::int32_t partition(std::int32_t first, std::int32_t count, const math::Aabb& box);
    std::int32_t build_node(std::int32_t first, std::int32_t count, std::int32_t parent);
    // From the node's own span of triangles. This is what the BUILD uses for
    // every node, leaf and internal alike, and it is deliberately not replaced
    // by `combine` below: a build has to stay bit-identical to what it produced
    // before this change, because `to_field` reads the winding number and there
    // are golden hashes downstream of it. `combine` is used only where the tree
    // is being refitted, where the spec already says the answer is equal to
    // tolerance rather than to the bit.
    void summarize_span(Node& n);
    // From two children, in constant time. The whole point of `area` and
    // `weighted_centroid` living on the node.
    void combine(Node& n, const Node& l, const Node& r);
    // `radius` from the node's box about its own centroid, which both of the
    // above finish with.
    void set_radius(Node& n);
    // The maps a refit needs, filled once at build. `slot_leaf_` is indexed by
    // position in `tris_`; `source_slot_` by the SOURCE triangle index, which
    // the build reorders away from.
    void index_for_refit(std::size_t source_triangles);
    bool refit_slots(const Mesh& m, const std::uint32_t* changed, std::size_t count);

    std::vector<Tri> tris_;
    // Mutable because the walks are const and this is measurement, not state:
    // a tree with a counter attached answers every query exactly as one
    // without.
    mutable BvhWalkStats* stats_ = nullptr;
    std::vector<Node> nodes_;
    // Source triangle -> index in `tris_`, or kNoSlot for one the build dropped
    // (an out-of-range index) and for the padding up to the mesh's count.
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
    std::vector<std::uint32_t> source_slot_;
    std::vector<std::int32_t> slot_leaf_;
    // Scratch for `refit`, kept so a per-stamp refit does not allocate. Sized
    // to the node count and reset through `dirty_list_` rather than cleared,
    // which is the same discipline `WalkScratch` uses in adjacency.h and for
    // the same reason: the reset must cost what was touched, not what exists.
    mutable std::vector<char> dirty_;
    mutable std::vector<std::int32_t> dirty_list_;
};

}  // namespace mesh
}  // namespace clay
