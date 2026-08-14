// Triangle BVH with winding-number summaries (meshing spec,
// add-mesh-to-field-import). See include/clay/mesh/bvh.h for why the sign
// comes from a winding number rather than a ray cast or a pseudonormal.

#include "clay/mesh/bvh.h"

#include <algorithm>
#include <cmath>

namespace clay {
namespace mesh {

using kernel::cf3;
using kernel::cfloat3;

namespace {

constexpr std::int32_t kLeafSize = 4;

float dot3(cfloat3 a, cfloat3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

cfloat3 cross3(cfloat3 a, cfloat3 b) {
    return cf3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

float length3(cfloat3 v) { return std::sqrt(dot3(v, v)); }

// Closest point on a triangle to p (Ericson, Real-Time Collision Detection).
// The seven cases are the three vertex regions, the three edge regions and the
// interior; each one returns as soon as it is identified.
cfloat3 closest_on_triangle(cfloat3 p, cfloat3 a, cfloat3 b, cfloat3 c) {
    cfloat3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    cfloat3 bp = p - b;
    float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    cfloat3 cp = p - c;
    float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

    float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// The solid angle a triangle subtends at the origin, with the vertices already
// made relative to the query point (Van Oosterom & Strackee). The atan2 form is
// used rather than a spherical-excess sum because it stays well conditioned as
// the triangle shrinks towards the query point, which is exactly where a narrow
// band puts its samples.
float solid_angle(cfloat3 a, cfloat3 b, cfloat3 c) {
    float la = length3(a), lb = length3(b), lc = length3(c);
    float numerator = dot3(a, cross3(b, c));
    float denominator =
        la * lb * lc + dot3(a, b) * lc + dot3(a, c) * lb + dot3(b, c) * la;
    if (numerator == 0.0f && denominator == 0.0f) return 0.0f;  // degenerate
    return 2.0f * std::atan2(numerator, denominator);
}

float squared_distance_to_box(const math::Aabb& box, cfloat3 p) {
    float total = 0.0f;
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    const float hi[3] = {box.max.x, box.max.y, box.max.z};
    const float q[3] = {p.x, p.y, p.z};
    for (int a = 0; a < 3; ++a) {
        float over = std::max(std::max(lo[a] - q[a], q[a] - hi[a]), 0.0f);
        total += over * over;
    }
    return total;
}

}  // namespace

Bvh Bvh::build(const Mesh& m) {
    Bvh bvh;
    const std::size_t count = m.triangle_count();
    bvh.tris_.reserve(count);
    for (std::size_t t = 0; t < count; ++t) {
        std::uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        const std::size_t n = m.positions.size();
        if (i0 >= n || i1 >= n || i2 >= n) continue;  // a bad index drops its triangle
        bvh.tris_.push_back(
            Tri{m.positions[i0], m.positions[i1], m.positions[i2], static_cast<std::uint32_t>(t)});
    }
    if (!bvh.tris_.empty()) bvh.build_node(0, static_cast<std::int32_t>(bvh.tris_.size()));
    return bvh;
}

// The winding-number summary of one node: the sum of its triangles'
// area-weighted normals, standing in for all of them when the node is far
// enough away to be treated as a single dipole.
void Bvh::summarize(Node& n) {
    cfloat3 normal_sum = cf3(0, 0, 0);
    cfloat3 weighted = cf3(0, 0, 0);
    float total_area = 0.0f;
    for (std::int32_t i = 0; i < n.span; ++i) {
        const Tri& t = tris_[static_cast<std::size_t>(n.first + i)];
        // Twice the area-weighted normal; the factor of two divides out of the
        // centroid and is folded into the dipole term at query time.
        cfloat3 cross = cross3(t.b - t.a, t.c - t.a);
        float area = length3(cross) * 0.5f;
        normal_sum = normal_sum + cross * 0.5f;
        weighted = weighted + (t.a + t.b + t.c) * (area / 3.0f);
        total_area += area;
    }
    n.normal_sum = normal_sum;
    n.centroid = total_area > 0.0f ? weighted * (1.0f / total_area)
                                   : (n.box.min + n.box.max) * 0.5f;
    // The radius of the node's box about that centroid, which is what the
    // far-field test compares the query distance against.
    cfloat3 corners[2] = {n.box.min, n.box.max};
    float furthest = 0.0f;
    for (int i = 0; i < 8; ++i) {
        cfloat3 corner = cf3(corners[i & 1].x, corners[(i >> 1) & 1].y, corners[(i >> 2) & 1].z);
        furthest = std::max(furthest, length3(corner - n.centroid));
    }
    n.radius = furthest;
}

// Median split on the longest axis, reordering `tris_` in place and returning
// how many landed on the left. Not a SAH build: this tree is built for one bake
// and then thrown away, so build time is part of what the caller pays and a
// cheaper tree wins overall.
std::int32_t Bvh::partition(std::int32_t first, std::int32_t count, const math::Aabb& box) {
    cfloat3 extent = box.max - box.min;
    const int axis =
        extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    auto centre = [axis](const Tri& t) {
        cfloat3 c = (t.a + t.b + t.c) * (1.0f / 3.0f);
        return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
    };
    auto begin = tris_.begin() + first;
    std::nth_element(begin, begin + count / 2, begin + count,
                     [&](const Tri& l, const Tri& r) { return centre(l) < centre(r); });
    return count / 2;
}

std::int32_t Bvh::build_node(std::int32_t first, std::int32_t count) {
    const std::int32_t self = static_cast<std::int32_t>(nodes_.size());
    nodes_.push_back(Node{});
    math::Aabb box;
    for (std::int32_t i = 0; i < count; ++i) {
        const Tri& t = tris_[static_cast<std::size_t>(first + i)];
        box.expand(t.a);
        box.expand(t.b);
        box.expand(t.c);
    }
    nodes_[static_cast<std::size_t>(self)].box = box;
    nodes_[static_cast<std::size_t>(self)].first = first;
    nodes_[static_cast<std::size_t>(self)].span = count;

    if (count > kLeafSize) {
        const std::int32_t half = partition(first, count, box);
        build_node(first, half);
        std::int32_t right = build_node(first + half, count - half);
        nodes_[static_cast<std::size_t>(self)].right = right;
        nodes_[static_cast<std::size_t>(self)].count = 0;
    } else {
        nodes_[static_cast<std::size_t>(self)].count = count;
    }
    summarize(nodes_[static_cast<std::size_t>(self)]);
    return self;
}

math::Aabb Bvh::bounds() const {
    return nodes_.empty() ? math::Aabb() : nodes_[0].box;
}

Bvh::RayHit Bvh::raycast(const math::Ray& ray, float tmin, float tmax) const {
    RayHit best;
    if (nodes_.empty()) return best;

    // Explicit stack rather than recursion: a query runs per Pencil event and
    // a deep tree on a scanned mesh should not be a stack depth question.
    std::int32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    float nearest = tmax;
    while (top > 0) {
        const std::int32_t self = stack[--top];
        const Node& n = nodes_[static_cast<std::size_t>(self)];
        float t0 = 0.0f, t1 = 0.0f;
        if (!math::ray_aabb(ray, n.box, &t0, &t1)) continue;
        if (t0 > nearest || t1 < tmin) continue;
        if (n.count > 0) {
            for (std::int32_t i = 0; i < n.count; ++i) {
                const Tri& tri = tris_[static_cast<std::size_t>(n.first + i)];
                // Möller–Trumbore, two-sided: the determinant's SIGN is not
                // tested, only its magnitude, so a back face hits too.
                const cfloat3 e1 = tri.b - tri.a, e2 = tri.c - tri.a;
                const cfloat3 pv = cross3(ray.dir, e2);
                const float det = dot3(e1, pv);
                if (std::fabs(det) < 1e-20f) continue;
                const float inv = 1.0f / det;
                const cfloat3 tv = ray.origin - tri.a;
                const float u = dot3(tv, pv) * inv;
                if (u < 0.0f || u > 1.0f) continue;
                const cfloat3 qv = cross3(tv, e1);
                const float v = dot3(ray.dir, qv) * inv;
                if (v < 0.0f || u + v > 1.0f) continue;
                const float t = dot3(e2, qv) * inv;
                if (t < tmin || t > nearest) continue;
                nearest = t;
                best = RayHit{true, t, tri.source, u, v};
            }
            continue;
        }
        // The build splits at the median, so the tree is balanced and 64 levels
        // covers more triangles than a machine can hold. The guard is a bound
        // on a corrupted tree, not a case that happens.
        if (top + 2 <= static_cast<int>(sizeof(stack) / sizeof(stack[0]))) {
            stack[top++] = self + 1;  // the left child is always the next node
            stack[top++] = n.right;
        }
    }
    return best;
}

float Bvh::unsigned_distance(cfloat3 p) const {
    if (nodes_.empty()) return 3.4e38f;
    float best = 3.4e38f;
    // Explicit stack, nearest child first: descending the closer subtree first
    // sets a tight bound early, which is what makes the far one prunable.
    std::vector<std::int32_t> stack;
    stack.push_back(0);
    while (!stack.empty()) {
        std::int32_t index = stack.back();
        stack.pop_back();
        const Node& n = nodes_[static_cast<std::size_t>(index)];
        if (squared_distance_to_box(n.box, p) >= best) continue;
        if (n.count > 0) {
            for (std::int32_t i = 0; i < n.count; ++i) {
                const Tri& t = tris_[static_cast<std::size_t>(n.first + i)];
                cfloat3 q = closest_on_triangle(p, t.a, t.b, t.c);
                best = std::min(best, dot3(p - q, p - q));
            }
            continue;
        }
        std::int32_t left = index + 1, right = n.right;
        float dl = squared_distance_to_box(nodes_[static_cast<std::size_t>(left)].box, p);
        float dr = squared_distance_to_box(nodes_[static_cast<std::size_t>(right)].box, p);
        // Pushed furthest-first so the nearer one is popped next.
        if (dl < dr) {
            stack.push_back(right);
            stack.push_back(left);
        } else {
            stack.push_back(left);
            stack.push_back(right);
        }
    }
    return std::sqrt(best);
}

float Bvh::winding_number(cfloat3 p, float beta) const {
    if (nodes_.empty()) return 0.0f;
    float total = 0.0f;
    std::vector<std::int32_t> stack;
    stack.push_back(0);
    while (!stack.empty()) {
        std::int32_t index = stack.back();
        stack.pop_back();
        const Node& n = nodes_[static_cast<std::size_t>(index)];

        // Far enough away to stand in for its triangles: one dipole term for
        // the whole subtree. beta <= 0 disables this, summing every triangle,
        // which is what the tests compare the approximation against.
        cfloat3 offset = n.centroid - p;
        float distance = length3(offset);
        if (beta > 0.0f && distance > beta * n.radius && distance > 0.0f) {
            total += dot3(n.normal_sum, offset) / (distance * distance * distance);
            continue;
        }
        if (n.count > 0) {
            for (std::int32_t i = 0; i < n.count; ++i) {
                const Tri& t = tris_[static_cast<std::size_t>(n.first + i)];
                total += solid_angle(t.a - p, t.b - p, t.c - p);
            }
            continue;
        }
        stack.push_back(index + 1);
        stack.push_back(n.right);
    }
    return total * 0.07957747f;  // 1 / (4 pi)
}

}  // namespace mesh
}  // namespace clay
