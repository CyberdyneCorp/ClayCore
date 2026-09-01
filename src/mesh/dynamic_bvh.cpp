#include "clay/mesh/dynamic_bvh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace clay {
namespace mesh {
namespace {

kernel::cfloat3 centroid_of(const DynamicSurface& s, FaceId f) {
    VertexId v[3];
    if (!s.face_vertices(f, v)) return kernel::cf3(0, 0, 0);
    return (s.position_of(v[0]) + s.position_of(v[1]) + s.position_of(v[2])) * (1.0f / 3.0f);
}

float aabb_distance2(const math::Aabb& b, kernel::cfloat3 p) {
    float d2 = 0.0f;
    const float px[3] = {p.x, p.y, p.z};
    const float lo[3] = {b.min.x, b.min.y, b.min.z};
    const float hi[3] = {b.max.x, b.max.y, b.max.z};
    for (int i = 0; i < 3; ++i) {
        const float v = px[i] < lo[i] ? lo[i] - px[i] : (px[i] > hi[i] ? px[i] - hi[i] : 0.0f);
        d2 += v * v;
    }
    return d2;
}

bool aabb_hits_ray(const math::Aabb& b, kernel::cfloat3 o, kernel::cfloat3 inv_d, float* out_t) {
    const float ox[3] = {o.x, o.y, o.z};
    const float id[3] = {inv_d.x, inv_d.y, inv_d.z};
    const float lo[3] = {b.min.x, b.min.y, b.min.z};
    const float hi[3] = {b.max.x, b.max.y, b.max.z};
    float tmin = 0.0f, tmax = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const float t0 = (lo[i] - ox[i]) * id[i];
        const float t1 = (hi[i] - ox[i]) * id[i];
        tmin = std::max(tmin, std::min(t0, t1));
        tmax = std::min(tmax, std::max(t0, t1));
    }
    *out_t = tmin;
    return tmax >= tmin;
}

// The closest point on a triangle to `p`, by the standard region test. Same
// shape the fixed BVH uses, so the two indices cannot disagree about what
// "nearest" means on the same geometry.
kernel::cfloat3 closest_on_triangle(kernel::cfloat3 p, kernel::cfloat3 a, kernel::cfloat3 b,
                                    kernel::cfloat3 c) {
    const kernel::cfloat3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = kernel::cdot(ab, ap), d2 = kernel::cdot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const kernel::cfloat3 bp = p - b;
    const float d3 = kernel::cdot(ab, bp), d4 = kernel::cdot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const kernel::cfloat3 cp = p - c;
    const float d5 = kernel::cdot(ab, cp), d6 = kernel::cdot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

bool ray_hits_triangle(kernel::cfloat3 o, kernel::cfloat3 d, kernel::cfloat3 a, kernel::cfloat3 b,
                       kernel::cfloat3 c, float* out_t) {
    const kernel::cfloat3 e1 = b - a, e2 = c - a;
    const kernel::cfloat3 pv = kernel::ccross(d, e2);
    const float det = kernel::cdot(e1, pv);
    if (std::fabs(det) < 1e-20f) return false;
    const float inv = 1.0f / det;
    const kernel::cfloat3 tv = o - a;
    const float u = kernel::cdot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const kernel::cfloat3 qv = kernel::ccross(tv, e1);
    const float v = kernel::cdot(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = kernel::cdot(e2, qv) * inv;
    if (t < 0.0f) return false;
    *out_t = t;
    return true;
}

}  // namespace

math::Aabb DynamicBvh::face_bounds(const DynamicSurface& s, FaceId f) const {
    math::Aabb b;
    VertexId v[3];
    if (!s.face_vertices(f, v)) return b;
    for (int i = 0; i < 3; ++i) b.expand(s.position_of(v[i]));
    return b;
}

void DynamicBvh::build(const DynamicSurface& surface, const DynamicBvhOptions& options) {
    options_ = options;
    leaves_.clear();
    nodes_.clear();
    dirty_.clear();
    dirty_epoch_.clear();
    face_leaf_.assign(surface.faces().capacity_slots(), kNoLeaf);
    epoch_ = 1;

    // Faces in SLOT ORDER, then partitioned spatially. The slot order is what
    // makes the partition a function of the surface rather than of the order
    // faces happened to be created in.
    std::vector<FaceId> all;
    all.reserve(surface.faces().size());
    surface.faces().for_each_live([&](FaceId id, const DynamicFace&) { all.push_back(id); });
    if (all.empty()) {
        root_ = 0xffffffffu;
        return;
    }

    // A median split down to leaves of the target size. Recursive over an index
    // range, so nothing is copied per level.
    struct Chunker {
        const DynamicSurface& s;
        DynamicBvh& self;
        std::vector<FaceId>& faces;
        std::vector<kernel::cfloat3>& centroids;

        void run(std::size_t begin, std::size_t end) {
            const std::size_t count = end - begin;
            if (count <= self.options_.target_leaf_faces) {
                SurfaceLeaf leaf;
                leaf.live = true;
                leaf.faces.assign(faces.begin() + static_cast<std::ptrdiff_t>(begin),
                                  faces.begin() + static_cast<std::ptrdiff_t>(end));
                for (FaceId f : leaf.faces) leaf.bounds.expand(self.face_bounds(s, f));
                const std::uint32_t index = static_cast<std::uint32_t>(self.leaves_.size());
                for (FaceId f : leaf.faces) {
                    if (f.slot >= self.face_leaf_.size())
                        self.face_leaf_.resize(f.slot + 1, kNoLeaf);
                    self.face_leaf_[f.slot] = index;
                }
                self.leaves_.push_back(std::move(leaf));
                return;
            }
            // Split on the widest axis of the centroid spread, at the median.
            math::Aabb spread;
            for (std::size_t i = begin; i < end; ++i) spread.expand(centroids[i]);
            const kernel::cfloat3 ext = spread.extent();
            const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
            const std::size_t mid = begin + count / 2;
            std::nth_element(
                faces.begin() + static_cast<std::ptrdiff_t>(begin),
                faces.begin() + static_cast<std::ptrdiff_t>(mid),
                faces.begin() + static_cast<std::ptrdiff_t>(end),
                [&](FaceId a, FaceId b) {
                    const kernel::cfloat3 ca = centroid_of(s, a), cb = centroid_of(s, b);
                    const float va = axis == 0 ? ca.x : (axis == 1 ? ca.y : ca.z);
                    const float vb = axis == 0 ? cb.x : (axis == 1 ? cb.y : cb.z);
                    // TIE-BROKEN BY SLOT, so the partition does not depend on
                    // the sort's own stability. `nth_element` is not stable, and
                    // a tie decided differently on another standard library
                    // would give a different chunking of the same surface.
                    if (va != vb) return va < vb;
                    return a.slot < b.slot;
                });
            // The centroids moved with the faces, so recompute the range.
            for (std::size_t i = begin; i < end; ++i) centroids[i] = centroid_of(s, faces[i]);
            run(begin, mid);
            run(mid, end);
        }
    };

    std::vector<kernel::cfloat3> centroids(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) centroids[i] = centroid_of(surface, all[i]);
    Chunker{surface, *this, all, centroids}.run(0, all.size());

    dirty_epoch_.assign(leaves_.size(), 0);
    rebuild_tree();
}

std::uint32_t DynamicBvh::build_node(std::vector<std::uint32_t>& order, std::size_t begin,
                                     std::size_t end, std::uint32_t parent) {
    const std::uint32_t index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[index].parent = parent;

    if (end - begin == 1) {
        const std::uint32_t leaf_index = order[begin];
        nodes_[index].leaf = leaf_index;
        nodes_[index].bounds = leaves_[leaf_index].bounds;
        leaves_[leaf_index].node = index;
        return index;
    }

    math::Aabb spread;
    for (std::size_t i = begin; i < end; ++i) spread.expand(leaves_[order[i]].bounds.center());
    const kernel::cfloat3 ext = spread.extent();
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    const std::size_t mid = begin + (end - begin) / 2;
    std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(begin),
                     order.begin() + static_cast<std::ptrdiff_t>(mid),
                     order.begin() + static_cast<std::ptrdiff_t>(end),
                     [&](std::uint32_t a, std::uint32_t b) {
                         const kernel::cfloat3 ca = leaves_[a].bounds.center();
                         const kernel::cfloat3 cb = leaves_[b].bounds.center();
                         const float va = axis == 0 ? ca.x : (axis == 1 ? ca.y : ca.z);
                         const float vb = axis == 0 ? cb.x : (axis == 1 ? cb.y : cb.z);
                         if (va != vb) return va < vb;
                         return a < b;  // deterministic tie-break, as above
                     });

    const std::uint32_t left = build_node(order, begin, mid, index);
    const std::uint32_t right = build_node(order, mid, end, index);
    nodes_[index].left = left;
    nodes_[index].right = right;
    nodes_[index].bounds = nodes_[left].bounds;
    nodes_[index].bounds.expand(nodes_[right].bounds);
    return index;
}

// Rebuild if a leaf split or merged since the last query. Called from the
// queries rather than from the mutations, so a stamp's thousands of inserts pay
// for one rebuild instead of thousands.
void DynamicBvh::ensure_tree() const {
    if (!tree_stale_) return;
    const_cast<DynamicBvh*>(this)->rebuild_tree();
}

void DynamicBvh::rebuild_tree() {
    nodes_.clear();
    std::vector<std::uint32_t> order;
    for (std::uint32_t i = 0; i < leaves_.size(); ++i)
        if (leaves_[i].live) order.push_back(i);
    if (order.empty()) {
        root_ = 0xffffffffu;
        tree_stale_ = false;
        return;
    }
    nodes_.reserve(order.size() * 2);
    root_ = build_node(order, 0, order.size(), 0xffffffffu);
    tree_stale_ = false;
}

void DynamicBvh::refit_ancestors(std::uint32_t node) {
    // UPWARD from the changed node, which is logarithmic in the number of
    // leaves. Refitting the whole tree would be correct and would make a dab
    // cost the surface, which is the one thing this index exists to avoid.
    while (node != 0xffffffffu) {
        Node& n = nodes_[node];
        if (n.leaf != kNoLeaf) {
            n.bounds = leaves_[n.leaf].bounds;
        } else {
            n.bounds = math::Aabb{};
            if (n.left != 0xffffffffu) n.bounds.expand(nodes_[n.left].bounds);
            if (n.right != 0xffffffffu) n.bounds.expand(nodes_[n.right].bounds);
        }
        node = n.parent;
    }
}

void DynamicBvh::refit_leaf(const DynamicSurface& surface, std::uint32_t leaf_index) {
    if (leaf_index >= leaves_.size() || !leaves_[leaf_index].live) return;
    SurfaceLeaf& leaf = leaves_[leaf_index];
    leaf.bounds = math::Aabb{};
    for (FaceId f : leaf.faces)
        if (surface.live(f)) leaf.bounds.expand(face_bounds(surface, f));
    if (leaf.node != 0xffffffffu) refit_ancestors(leaf.node);
}

void DynamicBvh::mark_dirty(std::uint32_t leaf_index, bool topology) {
    if (leaf_index >= leaves_.size()) return;
    if (dirty_epoch_.size() != leaves_.size()) dirty_epoch_.resize(leaves_.size(), 0);
    SurfaceLeaf& leaf = leaves_[leaf_index];
    leaf.revision = ++revision_;
    if (topology)
        leaf.topology_dirty = true;
    else
        leaf.geometry_dirty = true;
    // THE EPOCH MARK. A leaf already marked this epoch is already in the list,
    // so the list holds one entry per leaf however many times a stamp touches
    // it — and clearing is an increment rather than a walk.
    if (dirty_epoch_[leaf_index] == epoch_) return;
    dirty_epoch_[leaf_index] = epoch_;
    dirty_.push_back(leaf_index);
}

void DynamicBvh::clear_dirty() {
    for (std::uint32_t i : dirty_)
        if (i < leaves_.size()) {
            leaves_[i].geometry_dirty = false;
            leaves_[i].topology_dirty = false;
        }
    dirty_.clear();
    ++epoch_;
}

std::uint32_t DynamicBvh::choose_leaf(kernel::cfloat3 centroid) const {
    if (root_ == 0xffffffffu) return kNoLeaf;
    // Descend toward whichever child's bounds the centroid is nearer. Not a
    // surface-area heuristic: this runs per inserted face on the interactive
    // path, and the partition is repaired by a rebuild between strokes rather
    // than by being perfect during one.
    std::uint32_t node = root_;
    while (nodes_[node].leaf == kNoLeaf) {
        const std::uint32_t l = nodes_[node].left, r = nodes_[node].right;
        if (l == 0xffffffffu) {
            node = r;
            continue;
        }
        if (r == 0xffffffffu) {
            node = l;
            continue;
        }
        node = aabb_distance2(nodes_[l].bounds, centroid) <=
                       aabb_distance2(nodes_[r].bounds, centroid)
                   ? l
                   : r;
    }
    return nodes_[node].leaf;
}

void DynamicBvh::insert(const DynamicSurface& surface, FaceId face) {
    if (!surface.live(face)) return;
    if (face.slot >= face_leaf_.size()) face_leaf_.resize(face.slot + 1, kNoLeaf);
    if (face_leaf_[face.slot] != kNoLeaf) return;  // already indexed

    std::uint32_t leaf_index = choose_leaf(centroid_of(surface, face));
    if (leaf_index == kNoLeaf) {
        // The first face of an empty index.
        SurfaceLeaf leaf;
        leaf.live = true;
        leaves_.push_back(leaf);
        dirty_epoch_.push_back(0);
        leaf_index = static_cast<std::uint32_t>(leaves_.size() - 1);
        tree_stale_ = true;
    }
    leaves_[leaf_index].faces.push_back(face);
    leaves_[leaf_index].bounds.expand(face_bounds(surface, face));
    face_leaf_[face.slot] = leaf_index;
    mark_dirty(leaf_index, /*topology=*/true);
    if (leaves_[leaf_index].node != 0xffffffffu) refit_ancestors(leaves_[leaf_index].node);

    if (leaves_[leaf_index].faces.size() > options_.max_leaf_faces)
        split_leaf(surface, leaf_index);
    // THE TREE IS NOT REBUILT HERE. `insert` runs once per face per topology
    // operation, and a stamp on a big surface runs thousands of them; rebuilding
    // the tree over the leaves inside it made the stamp O(operations x leaves)
    // and turned a 4x bigger model into a 40x slower dab. The rebuild is
    // deferred to the next QUERY, which needs it and happens once.
}

void DynamicBvh::split_leaf(const DynamicSurface& surface, std::uint32_t leaf_index) {
    SurfaceLeaf& leaf = leaves_[leaf_index];
    if (leaf.faces.size() < 2) return;

    // Median split on the widest axis, same rule as the build, with the same
    // slot tie-break so a leaf split during a stroke partitions the way a
    // rebuild would have.
    math::Aabb spread;
    for (FaceId f : leaf.faces) spread.expand(centroid_of(surface, f));
    const kernel::cfloat3 ext = spread.extent();
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    std::sort(leaf.faces.begin(), leaf.faces.end(), [&](FaceId a, FaceId b) {
        const kernel::cfloat3 ca = centroid_of(surface, a), cb = centroid_of(surface, b);
        const float va = axis == 0 ? ca.x : (axis == 1 ? ca.y : ca.z);
        const float vb = axis == 0 ? cb.x : (axis == 1 ? cb.y : cb.z);
        if (va != vb) return va < vb;
        return a.slot < b.slot;
    });

    SurfaceLeaf other;
    other.live = true;
    const std::size_t mid = leaf.faces.size() / 2;
    other.faces.assign(leaf.faces.begin() + static_cast<std::ptrdiff_t>(mid), leaf.faces.end());
    leaf.faces.resize(mid);

    const std::uint32_t other_index = static_cast<std::uint32_t>(leaves_.size());
    for (FaceId f : other.faces) {
        if (f.slot < face_leaf_.size()) face_leaf_[f.slot] = other_index;
        other.bounds.expand(face_bounds(surface, f));
    }
    leaves_[leaf_index].bounds = math::Aabb{};
    for (FaceId f : leaves_[leaf_index].faces)
        leaves_[leaf_index].bounds.expand(face_bounds(surface, f));

    leaves_.push_back(std::move(other));
    dirty_epoch_.push_back(0);
    mark_dirty(other_index, /*topology=*/true);
    // The leaf SET changed, so the tree over the leaves has to be rebuilt. That
    // is O(leaves), not O(faces), and it happens once per few hundred inserted
    // faces rather than per dab.
    tree_stale_ = true;
}

void DynamicBvh::erase(FaceId face) {
    if (face.slot >= face_leaf_.size()) return;
    const std::uint32_t leaf_index = face_leaf_[face.slot];
    if (leaf_index == kNoLeaf || leaf_index >= leaves_.size()) return;
    SurfaceLeaf& leaf = leaves_[leaf_index];
    for (std::size_t i = 0; i < leaf.faces.size(); ++i)
        if (leaf.faces[i].slot == face.slot) {
            leaf.faces[i] = leaf.faces.back();
            leaf.faces.pop_back();
            break;
        }
    face_leaf_[face.slot] = kNoLeaf;
    mark_dirty(leaf_index, /*topology=*/true);
    // The bounds are left as they are: they still CONTAIN everything in the
    // leaf, which is all a bounding volume promises. Shrinking them costs a
    // pass over the leaf and buys tightness that the next rebuild gives for
    // free.
}

void DynamicBvh::update(const DynamicSurface& surface, FaceId face) {
    if (face.slot >= face_leaf_.size()) return;
    const std::uint32_t leaf_index = face_leaf_[face.slot];
    if (leaf_index == kNoLeaf || leaf_index >= leaves_.size()) return;
    leaves_[leaf_index].bounds.expand(face_bounds(surface, face));
    mark_dirty(leaf_index, /*topology=*/false);
    if (leaves_[leaf_index].node != 0xffffffffu) refit_ancestors(leaves_[leaf_index].node);
}

void DynamicBvh::update_many(const DynamicSurface& surface, const std::vector<FaceId>& faces) {
    // One refit pass per LEAF rather than per face: a stamp usually touches a
    // few hundred faces in one or two leaves, and refitting the ancestors once
    // per face would multiply the logarithmic cost by the footprint.
    //
    // A MEMBER rather than a local, for the reason every other scratch buffer
    // on a per-stamp path is one: this runs once per stamp, and a local here
    // allocated and freed on every dab.
    std::vector<std::uint32_t>& touched = update_scratch_;
    touched.clear();
    for (FaceId f : faces) {
        if (f.slot >= face_leaf_.size()) continue;
        const std::uint32_t leaf_index = face_leaf_[f.slot];
        if (leaf_index == kNoLeaf || leaf_index >= leaves_.size()) continue;
        if (surface.live(f)) leaves_[leaf_index].bounds.expand(face_bounds(surface, f));
        mark_dirty(leaf_index, /*topology=*/false);
        touched.push_back(leaf_index);
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (std::uint32_t i : touched)
        if (leaves_[i].node != 0xffffffffu) refit_ancestors(leaves_[i].node);
}

// -- queries ------------------------------------------------------------------

namespace {

// The stack a query walks the tree with: INLINE, with a heap tail only for a
// tree deeper than any realistic one.
//
// Each of the three queries below used to build a `std::vector` for this, which
// is an allocation on every pick, every ball query and every seed resolution —
// and, growing by doubling from one entry, up to a dozen of them. An adaptive
// stamp makes three such calls, so this was most of what a stamp allocated
// after everything else had been made to allocate nothing.
//
// The bound is about DEPTH rather than size: a node is pushed only when its
// parent is popped, so the stack holds at most one sibling per level of the
// path being walked. Ninety-six levels is far past any tree this library
// builds, and past it the spill is correct rather than merely safe.
class TraversalStack {
   public:
    explicit TraversalStack(std::uint32_t root) { push(root); }

    bool empty() const { return size_ == 0; }
    void push(std::uint32_t node) {
        if (size_ < kInline)
            inline_[size_] = node;
        else
            spill_.push_back(node);
        ++size_;
    }
    std::uint32_t pop() {
        --size_;
        if (size_ < kInline) return inline_[size_];
        const std::uint32_t node = spill_.back();
        spill_.pop_back();
        return node;
    }

   private:
    static constexpr std::size_t kInline = 96;
    std::uint32_t inline_[kInline] = {};
    std::vector<std::uint32_t> spill_;
    std::size_t size_ = 0;
};

}  // namespace


void DynamicBvh::faces_in_ball(const DynamicSurface& surface, kernel::cfloat3 centre, float radius,
                               std::vector<FaceId>* out) const {
    out->clear();
    ensure_tree();
    if (root_ == 0xffffffffu || radius <= 0.0f) return;
    const float r2 = radius * radius;
    TraversalStack stack(root_);
    while (!stack.empty()) {
        const std::uint32_t node = stack.pop();
        const Node& n = nodes_[node];
        if (aabb_distance2(n.bounds, centre) > r2) continue;
        if (n.leaf == kNoLeaf) {
            if (n.left != 0xffffffffu) stack.push(n.left);
            if (n.right != 0xffffffffu) stack.push(n.right);
            continue;
        }
        // OVER-ADMITTED at the leaf, EXACT at the face: a leaf whose bounds
        // reach the ball may hold faces that do not.
        for (FaceId f : leaves_[n.leaf].faces) {
            if (!surface.live(f)) continue;
            VertexId v[3];
            if (!surface.face_vertices(f, v)) continue;
            const kernel::cfloat3 p = closest_on_triangle(centre, surface.position_of(v[0]),
                                                          surface.position_of(v[1]),
                                                          surface.position_of(v[2]));
            if (kernel::cdot2(p - centre) <= r2) out->push_back(f);
        }
    }
    // SORTED BY SLOT before it is handed back, so a caller's own order does not
    // depend on the tree's shape — and the tree's shape depends on the history
    // of edits. The determinism rule reaches the query results too.
    std::sort(out->begin(), out->end(), [](FaceId a, FaceId b) { return a.slot < b.slot; });
}

DynamicBvh::ClosestPoint DynamicBvh::closest(const DynamicSurface& surface,
                                             kernel::cfloat3 p) const {
    ClosestPoint out;
    ensure_tree();
    if (root_ == 0xffffffffu) return out;
    float best2 = std::numeric_limits<float>::max();
    TraversalStack stack(root_);
    while (!stack.empty()) {
        const std::uint32_t node = stack.pop();
        const Node& n = nodes_[node];
        if (aabb_distance2(n.bounds, p) > best2) continue;
        if (n.leaf == kNoLeaf) {
            if (n.left != 0xffffffffu) stack.push(n.left);
            if (n.right != 0xffffffffu) stack.push(n.right);
            continue;
        }
        for (FaceId f : leaves_[n.leaf].faces) {
            if (!surface.live(f)) continue;
            VertexId v[3];
            if (!surface.face_vertices(f, v)) continue;
            const kernel::cfloat3 q = closest_on_triangle(p, surface.position_of(v[0]),
                                                          surface.position_of(v[1]),
                                                          surface.position_of(v[2]));
            const float d2 = kernel::cdot2(q - p);
            // Ties broken by SLOT, so two equidistant faces resolve the same
            // way on every run.
            if (d2 < best2 || (d2 == best2 && out.found && f.slot < out.face.slot)) {
                best2 = d2;
                out.found = true;
                out.face = f;
                out.position = q;
            }
        }
    }
    if (out.found) out.distance = std::sqrt(best2);
    return out;
}

DynamicBvh::RayHit DynamicBvh::raycast(const DynamicSurface& surface, kernel::cfloat3 origin,
                                       kernel::cfloat3 direction) const {
    RayHit out;
    ensure_tree();
    if (root_ == 0xffffffffu) return out;
    const float len = kernel::clength(direction);
    if (len < 1e-20f) return out;
    const kernel::cfloat3 d = direction / len;
    const kernel::cfloat3 inv = kernel::cf3(1.0f / (d.x == 0.0f ? 1e-20f : d.x),
                                            1.0f / (d.y == 0.0f ? 1e-20f : d.y),
                                            1.0f / (d.z == 0.0f ? 1e-20f : d.z));
    float best = std::numeric_limits<float>::max();
    TraversalStack stack(root_);
    while (!stack.empty()) {
        const std::uint32_t node = stack.pop();
        const Node& n = nodes_[node];
        float enter = 0.0f;
        if (!aabb_hits_ray(n.bounds, origin, inv, &enter) || enter > best) continue;
        if (n.leaf == kNoLeaf) {
            if (n.left != 0xffffffffu) stack.push(n.left);
            if (n.right != 0xffffffffu) stack.push(n.right);
            continue;
        }
        for (FaceId f : leaves_[n.leaf].faces) {
            if (!surface.live(f)) continue;
            VertexId v[3];
            if (!surface.face_vertices(f, v)) continue;
            float t = 0.0f;
            if (!ray_hits_triangle(origin, d, surface.position_of(v[0]), surface.position_of(v[1]),
                                   surface.position_of(v[2]), &t))
                continue;
            if (t < best || (t == best && out.hit && f.slot < out.face.slot)) {
                best = t;
                out.hit = true;
                out.face = f;
                out.t = t;
            }
        }
    }
    if (out.hit) out.position = origin + d * out.t;
    return out;
}

// -- introspection ------------------------------------------------------------

std::size_t DynamicBvh::leaf_count() const {
    std::size_t n = 0;
    for (const SurfaceLeaf& l : leaves_)
        if (l.live) ++n;
    return n;
}

const SurfaceLeaf* DynamicBvh::leaf(std::uint32_t index) const {
    if (index >= leaves_.size() || !leaves_[index].live) return nullptr;
    return &leaves_[index];
}

std::uint32_t DynamicBvh::leaf_of(FaceId face) const {
    if (face.slot >= face_leaf_.size()) return kNoLeaf;
    return face_leaf_[face.slot];
}

math::Aabb DynamicBvh::bounds() const {
    if (root_ == 0xffffffffu) return math::Aabb{};
    return nodes_[root_].bounds;
}

float DynamicBvh::quality() const {
    // The mean leaf volume against the volume of their union. One means a
    // perfect partition; larger means the leaves overlap, which is what local
    // edits do to a tree over time.
    if (leaves_.empty()) return 1.0f;
    const math::Aabb whole = bounds();
    const kernel::cfloat3 we = whole.extent();
    const float total = std::max(we.x * we.y * we.z, 1e-20f);
    float sum = 0.0f;
    std::size_t n = 0;
    for (const SurfaceLeaf& l : leaves_) {
        if (!l.live || l.faces.empty()) continue;
        const kernel::cfloat3 e = l.bounds.extent();
        sum += std::max(e.x * e.y * e.z, 0.0f);
        ++n;
    }
    if (n == 0) return 1.0f;
    return sum / total;
}

bool DynamicBvh::wants_rebuild() const {
    // ADVISORY. Nothing here rebuilds on its own behalf, for the reason
    // `Bvh::quality` records: over five measured deformations a rebuild
    // produced a better tree in exactly one and a dramatically worse one in
    // two. The caller rebuilds BETWEEN strokes if it wants to.
    return quality() > 3.0f;
}

std::size_t DynamicBvh::bytes() const {
    std::size_t n = sizeof(DynamicBvh);
    n += nodes_.capacity() * sizeof(Node);
    n += leaves_.capacity() * sizeof(SurfaceLeaf);
    for (const SurfaceLeaf& l : leaves_) n += l.faces.capacity() * sizeof(FaceId);
    n += face_leaf_.capacity() * sizeof(std::uint32_t);
    n += (dirty_.capacity() + dirty_epoch_.capacity()) * sizeof(std::uint32_t);
    return n;
}

}  // namespace mesh
}  // namespace clay
