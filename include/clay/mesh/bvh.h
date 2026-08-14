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

class Bvh {
  public:
    // Build over `m`'s triangles. The mesh is not retained: the triangles are
    // copied in, so the caller may free or edit the mesh afterwards.
    static Bvh build(const Mesh& m);

    bool empty() const { return tris_.empty(); }
    std::size_t triangle_count() const { return tris_.size(); }
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
        float radius = 0.0f;  // of `box` about `centroid`
        std::int32_t first = 0;
        std::int32_t span = 0;    // triangles beneath this node, leaf or not
        std::int32_t count = 0;   // > 0 only for a leaf: triangles held here
        std::int32_t right = -1;  // left child is this + 1
    };

    std::int32_t partition(std::int32_t first, std::int32_t count, const math::Aabb& box);
    std::int32_t build_node(std::int32_t first, std::int32_t count);
    void summarize(Node& n);

    std::vector<Tri> tris_;
    std::vector<Node> nodes_;
};

}  // namespace mesh
}  // namespace clay
