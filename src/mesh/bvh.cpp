// Triangle BVH with winding-number summaries (meshing spec,
// add-mesh-to-field-import). See include/clay/mesh/bvh.h for why the sign
// comes from a winding number rather than a ray cast or a pseudonormal.

#include "clay/mesh/bvh.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

// Barycentrics of a point already known to lie in a triangle's plane, by the
// projected-area method. A degenerate triangle has no basis to express them in
// and reports the first corner, which is the only answer that is on the
// triangle at all.
void barycentric(cfloat3 p, cfloat3 a, cfloat3 b, cfloat3 c, float* u, float* v) {
    cfloat3 v0 = b - a, v1 = c - a, v2 = p - a;
    float d00 = dot3(v0, v0), d01 = dot3(v0, v1), d11 = dot3(v1, v1);
    float d20 = dot3(v2, v0), d21 = dot3(v2, v1);
    float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-20f) {
        *u = 0.0f;
        *v = 0.0f;
        return;
    }
    *u = (d11 * d20 - d01 * d21) / denom;
    *v = (d00 * d21 - d01 * d20) / denom;
}

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
    if (!bvh.tris_.empty()) bvh.build_node(0, static_cast<std::int32_t>(bvh.tris_.size()), -1);
    bvh.index_for_refit(count);
    return bvh;
}

// The winding-number summary of one node: the sum of its triangles'
// area-weighted normals, standing in for all of them when the node is far
// enough away to be treated as a single dipole.
void Bvh::summarize_span(Node& n) {
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
    // Kept rather than discarded, so a refit can rebuild a parent from its two
    // children instead of from its span. Everything below reads them; nothing
    // in the BUILD does, which is what keeps a built tree bit-identical.
    n.weighted_centroid = weighted;
    n.area = total_area;
    n.centroid = total_area > 0.0f ? weighted * (1.0f / total_area)
                                   : (n.box.min + n.box.max) * 0.5f;
    set_radius(n);
}

// The parent of two summarised children, in constant time.
//
// The box is the union of the two, which is EXACT — a union of unions is the
// same union, and min and max do not round. The dipole is not: floating-point
// addition is not associative, so `left + right` differs in the last bits from
// summing the span. That is the whole of the difference between a refitted tree
// and a rebuilt one, and the tests say so rather than asserting bit-identity
// they cannot have.
void Bvh::combine(Node& n, const Node& l, const Node& r) {
    n.box = math::Aabb();
    n.box.expand(l.box.min);
    n.box.expand(l.box.max);
    n.box.expand(r.box.min);
    n.box.expand(r.box.max);
    n.normal_sum = l.normal_sum + r.normal_sum;
    n.weighted_centroid = l.weighted_centroid + r.weighted_centroid;
    n.area = l.area + r.area;
    // The degenerate branch has to be taken explicitly rather than inherited: a
    // subtree of zero-area triangles has no weighted centroid to divide, and
    // `summarize_span` answers with the box centre. A parent combining one such
    // child with a normal one is fine — the zero child contributes nothing to
    // either sum — but a parent whose whole subtree is degenerate is not.
    n.centroid = n.area > 0.0f ? n.weighted_centroid * (1.0f / n.area)
                               : (n.box.min + n.box.max) * 0.5f;
    set_radius(n);
}

// The radius of the node's box about its own centroid, which is what the
// far-field test compares the query distance against.
void Bvh::set_radius(Node& n) {
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

// Nodes are appended in DFS PRE-ORDER — self, then left, then right — so a
// parent's index is always lower than either child's. `refit` leans on that:
// walking the node array from the end backwards visits every child before its
// parent, which is the whole of the ordering problem, with no queue and no sort.
std::int32_t Bvh::build_node(std::int32_t first, std::int32_t count, std::int32_t parent) {
    const std::int32_t self = static_cast<std::int32_t>(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[static_cast<std::size_t>(self)].parent = parent;
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
        build_node(first, half, self);
        std::int32_t right = build_node(first + half, count - half, self);
        nodes_[static_cast<std::size_t>(self)].right = right;
        nodes_[static_cast<std::size_t>(self)].count = 0;
    } else {
        nodes_[static_cast<std::size_t>(self)].count = count;
    }
    summarize_span(nodes_[static_cast<std::size_t>(self)]);
    return self;
}

// The two maps a refit needs, filled once here because both are byproducts of a
// build that would otherwise be thrown away.
//
// `source_slot_` exists because `partition` reorders `tris_` with nth_element,
// so the triangle a caller names by its mesh index is not at that index here.
// `slot_leaf_` exists because a changed triangle has to reach the node holding
// it, and a leaf owns a contiguous span, so one pass over the leaves fills it.
void Bvh::index_for_refit(std::size_t source_triangles) {
    source_slot_.assign(source_triangles, kNoSlot);
    slot_leaf_.assign(tris_.size(), -1);
    for (std::size_t s = 0; s < tris_.size(); ++s) {
        const std::uint32_t src = tris_[s].source;
        if (src < source_slot_.size()) source_slot_[src] = static_cast<std::uint32_t>(s);
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        if (n.count <= 0) continue;  // internal
        for (std::int32_t k = 0; k < n.count; ++k)
            slot_leaf_[static_cast<std::size_t>(n.first + k)] = static_cast<std::int32_t>(i);
    }
    dirty_.assign(nodes_.size(), 0);
    dirty_list_.clear();
}

// Copy the named triangles' positions in from the mesh and mark the leaves that
// hold them. Returns false when the mesh is not the one this tree was built
// over, before touching anything.
bool Bvh::refit_slots(const Mesh& m, const std::uint32_t* changed, std::size_t count) {
    if (m.triangle_count() != source_slot_.size()) return false;
    if (nodes_.empty()) return true;  // an empty tree is already fitted
    // Reset through the LIST rather than by clearing the array. Clearing costs
    // O(nodes), which is O(mesh) — the exact shape of defect issue #192 is
    // about, and it would sit on the per-stamp path this whole change exists to
    // make proportional to the brush. The list holds precisely what the last
    // refit marked, so the reset costs what was touched. Same discipline as
    // `WalkScratch` in adjacency.h, for the same reason.
    if (dirty_.size() != nodes_.size()) {
        dirty_.assign(nodes_.size(), 0);
        dirty_list_.clear();
    } else {
        for (std::int32_t n : dirty_list_) dirty_[static_cast<std::size_t>(n)] = 0;
        dirty_list_.clear();
    }
    const std::size_t verts = m.positions.size();
    for (std::size_t k = 0; k < count; ++k) {
        const std::uint32_t src = changed ? changed[k] : static_cast<std::uint32_t>(k);
        if (src >= source_slot_.size()) continue;
        const std::uint32_t slot = source_slot_[src];
        if (slot == kNoSlot) continue;  // dropped at build time, by a bad index
        const std::uint32_t i0 = m.indices[static_cast<std::size_t>(src) * 3];
        const std::uint32_t i1 = m.indices[static_cast<std::size_t>(src) * 3 + 1];
        const std::uint32_t i2 = m.indices[static_cast<std::size_t>(src) * 3 + 2];
        if (i0 >= verts || i1 >= verts || i2 >= verts) continue;
        Tri& t = tris_[slot];
        t.a = m.positions[i0];
        t.b = m.positions[i1];
        t.c = m.positions[i2];
        // Mark the leaf and every ancestor. The walk stops at the first node
        // already marked, because everything above it was marked by whoever
        // marked it — which is what keeps the marking proportional to the
        // distinct paths touched rather than to (triangles x depth).
        std::int32_t node = slot_leaf_[slot];
        while (node >= 0 && !dirty_[static_cast<std::size_t>(node)]) {
            dirty_[static_cast<std::size_t>(node)] = 1;
            dirty_list_.push_back(node);
            node = nodes_[static_cast<std::size_t>(node)].parent;
        }
    }
    return true;
}

bool Bvh::refit(const Mesh& m, const std::uint32_t* changed, std::size_t count) {
    if (!refit_slots(m, changed, count)) return false;
    if (dirty_list_.empty()) return true;
    // Descending node index visits every child before its parent — see the note
    // on build_node. Sorting the dirty list is what makes that true for a set
    // marked in arbitrary order; it is over the nodes TOUCHED, not over the
    // tree.
    std::sort(dirty_list_.begin(), dirty_list_.end());
    for (std::size_t i = dirty_list_.size(); i-- > 0;) {
        Node& n = nodes_[static_cast<std::size_t>(dirty_list_[i])];
        if (n.count > 0) {
            // A leaf: its own triangles are the only source of truth, and there
            // are at most kLeafSize of them.
            n.box = math::Aabb();
            for (std::int32_t k = 0; k < n.count; ++k) {
                const Tri& t = tris_[static_cast<std::size_t>(n.first + k)];
                n.box.expand(t.a);
                n.box.expand(t.b);
                n.box.expand(t.c);
            }
            summarize_span(n);
        } else {
            const std::int32_t self = dirty_list_[i];
            combine(n, nodes_[static_cast<std::size_t>(self + 1)],
                    nodes_[static_cast<std::size_t>(n.right)]);
        }
    }
    return true;
}

bool Bvh::refit(const Mesh& m) {
    return refit(m, nullptr, m.triangle_count());
}

float Bvh::quality() const {
    // The surface-area heuristic's own cost estimate, read rather than
    // optimised: the expected number of triangle tests a random ray through
    // the root box has to make, being the sum over LEAVES of their surface area
    // times how many triangles they hold, divided by the root's area.
    //
    // Accumulated in DOUBLE. In float the products overflow on a large model —
    // a million leaves of a few square units each is already past 1e38 — and
    // the answer comes back inf or, worse, a finite number that has lost its
    // low bits. The inputs are float and the ratio is float; only the sum needs
    // the width.
    //
    // The obvious alternative — the mean over internal nodes of (child area sum
    // / own area) — was tried and rejected by measurement: it averages the few
    // nodes a brush stretched against the thousands it did not, so a pull of
    // twenty units moved it from 1.153 to 1.170, and at one unit it scored the
    // stretched tree BETTER than a rebuild. Nothing a host could threshold.
    auto area_of = [](const math::Aabb& b) {
        const double x = std::max(static_cast<double>(b.max.x) - static_cast<double>(b.min.x), 0.0);
        const double y = std::max(static_cast<double>(b.max.y) - static_cast<double>(b.min.y), 0.0);
        const double z = std::max(static_cast<double>(b.max.z) - static_cast<double>(b.min.z), 0.0);
        return 2.0 * (x * y + y * z + z * x);
    };
    if (nodes_.empty()) return 0.0f;
    const double root = area_of(nodes_[0].box);
    // A root with no surface area — every triangle in one plane, or a single
    // degenerate one — has nothing to normalise by. NaN rather than 0, because
    // 0 is the BEST end of this scale and would read as "queries got cheaper"
    // for a tree nothing has been measured about. Every comparison against a
    // NaN is false, which is the honest answer to a question with no answer.
    if (!(root > 0.0)) return std::numeric_limits<float>::quiet_NaN();
    double cost = 0.0;
    for (const Node& n : nodes_) {
        if (n.count <= 0) continue;  // internal nodes carry no triangle tests
        cost += area_of(n.box) * static_cast<double>(n.count);
    }
    return static_cast<float>(cost / root);
}

bool Bvh::bounds_contain_their_triangles(const Mesh* m) const {
    auto holds = [](const math::Aabb& b, cfloat3 p) {
        return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y &&
               p.z >= b.min.z && p.z <= b.max.z;
    };
    auto same = [](cfloat3 a, cfloat3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    for (const Node& n : nodes_)
        for (std::int32_t i = 0; i < n.span; ++i) {
            const Tri& t = tris_[static_cast<std::size_t>(n.first + i)];
            if (!holds(n.box, t.a) || !holds(n.box, t.b) || !holds(n.box, t.c)) return false;
            if (!m) continue;
            // The half that self-consistency cannot see: does the tree still
            // hold the mesh's CURRENT geometry? A refit given a subset leaves
            // this false and everything above it true.
            const std::size_t base = static_cast<std::size_t>(t.source) * 3;
            if (base + 2 >= m->indices.size()) return false;
            const std::uint32_t i0 = m->indices[base], i1 = m->indices[base + 1];
            const std::uint32_t i2 = m->indices[base + 2];
            const std::size_t verts = m->positions.size();
            if (i0 >= verts || i1 >= verts || i2 >= verts) return false;
            if (!same(t.a, m->positions[i0]) || !same(t.b, m->positions[i1]) ||
                !same(t.c, m->positions[i2]))
                return false;
        }
    return true;
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

Bvh::ClosestPoint Bvh::closest(cfloat3 p) const {
    ClosestPoint best;
    if (nodes_.empty()) return best;
    float best_d2 = 3.4e38f;
    // Explicit stack, nearest child first: descending the closer subtree first
    // sets a tight bound early, which is what makes the far one prunable.
    std::vector<std::int32_t> stack;
    stack.push_back(0);
    while (!stack.empty()) {
        std::int32_t index = stack.back();
        stack.pop_back();
        const Node& n = nodes_[static_cast<std::size_t>(index)];
        if (squared_distance_to_box(n.box, p) >= best_d2) continue;
        if (n.count > 0) {
            for (std::int32_t i = 0; i < n.count; ++i) {
                const Tri& t = tris_[static_cast<std::size_t>(n.first + i)];
                cfloat3 q = closest_on_triangle(p, t.a, t.b, t.c);
                float d2 = dot3(p - q, p - q);
                // Strictly less, so a tie keeps the triangle found first and
                // which one that is does not depend on the traversal order.
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best.found = true;
                    best.point = q;
                    best.triangle = t.source;
                    barycentric(q, t.a, t.b, t.c, &best.u, &best.v);
                }
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
    if (best.found) best.distance = std::sqrt(best_d2);
    return best;
}

float Bvh::unsigned_distance(cfloat3 p) const {
    const ClosestPoint hit = closest(p);
    // The same 3.4e38 an empty tree reported when this was its own traversal.
    return hit.found ? hit.distance : 3.4e38f;
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
