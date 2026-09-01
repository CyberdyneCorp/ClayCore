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
    nodes_.clear();
    ChunkOptions chunking;
    chunking.target_faces = options_.target_leaf_faces;
    chunking.max_faces = options_.max_leaf_faces;
    chunking.min_faces = options_.min_leaf_faces;
    // Sized from the surface rather than grown: the partition is a function of
    // the surface, so its arena is one allocation and not one per chunk.
    const std::size_t faces = surface.faces().size();
    const std::size_t expected_chunks =
        faces / std::max<std::size_t>(chunking.target_faces, 1) + 1;
    table_.reset(expected_chunks, faces + expected_chunks * 16);
    table_.set_options(chunking);
    face_leaf_.assign(surface.faces().capacity_slots(), kNoLeaf);

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
                const std::uint32_t index = self.table_.create();
                self.table_.assign_faces(index, faces.data() + begin, count);
                math::Aabb bounds;
                for (std::size_t i = begin; i < end; ++i)
                    bounds.expand(self.face_bounds(s, faces[i]));
                self.table_.set_bounds(index, bounds);
                for (std::size_t i = begin; i < end; ++i) {
                    const FaceId f = faces[i];
                    if (f.slot >= self.face_leaf_.size())
                        self.face_leaf_.resize(f.slot + 1, kNoLeaf);
                    self.face_leaf_[f.slot] = index;
                }
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

    // A fresh partition is not a change a host has to redraw: it is the state
    // the host is about to read for the first time.
    table_.clear_dirty();
    rebuild_tree();
}

std::uint32_t DynamicBvh::build_node(std::vector<std::uint32_t>& order, std::size_t begin,
                                     std::size_t end, std::uint32_t parent) {
    const std::uint32_t index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[index].parent = parent;

    if (end - begin == 1) {
        const std::uint32_t leaf_index = order[begin];
        SurfaceLeaf* leaf = table_.chunk_mutable(leaf_index);
        nodes_[index].leaf = leaf_index;
        nodes_[index].bounds = leaf->bounds;
        leaf->node = index;
        return index;
    }

    math::Aabb spread;
    for (std::size_t i = begin; i < end; ++i)
        spread.expand(table_.chunk(order[i])->bounds.center());
    const kernel::cfloat3 ext = spread.extent();
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    const std::size_t mid = begin + (end - begin) / 2;
    std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(begin),
                     order.begin() + static_cast<std::ptrdiff_t>(mid),
                     order.begin() + static_cast<std::ptrdiff_t>(end),
                     [&](std::uint32_t a, std::uint32_t b) {
                         const kernel::cfloat3 ca = table_.chunk(a)->bounds.center();
                         const kernel::cfloat3 cb = table_.chunk(b)->bounds.center();
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
    for (std::uint32_t i = 0; i < table_.slot_count(); ++i)
        if (table_.chunk(i) != nullptr) order.push_back(i);
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
            n.bounds = table_.chunk(n.leaf)->bounds;
        } else {
            n.bounds = math::Aabb{};
            if (n.left != 0xffffffffu) n.bounds.expand(nodes_[n.left].bounds);
            if (n.right != 0xffffffffu) n.bounds.expand(nodes_[n.right].bounds);
        }
        node = n.parent;
    }
}

void DynamicBvh::refit_leaf(const DynamicSurface& surface, std::uint32_t leaf_index) {
    SurfaceLeaf* leaf = table_.chunk_mutable(leaf_index);
    if (leaf == nullptr) return;
    leaf->bounds = math::Aabb{};
    for (FaceId f : leaf->faces)
        if (surface.live(f)) leaf->bounds.expand(face_bounds(surface, f));
    if (leaf->node != 0xffffffffu) refit_ancestors(leaf->node);
}

void DynamicBvh::mark_dirty(std::uint32_t leaf_index, bool topology) {
    // THE EPOCH MARK lives in the table now, with the four revisions and the
    // per-chunk acknowledgement. What is decided here is only WHICH of the four
    // a tree operation advanced: membership, or the same faces in new places.
    table_.mark(leaf_index, topology ? ChunkDirty::Topology : ChunkDirty::Geometry);
}

void DynamicBvh::clear_dirty() { table_.clear_dirty(); }

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
        leaf_index = table_.create();
        tree_stale_ = true;
    }
    table_.add_face(leaf_index, face);
    table_.expand_bounds(leaf_index, face_bounds(surface, face));
    face_leaf_[face.slot] = leaf_index;
    mark_dirty(leaf_index, /*topology=*/true);
    const SurfaceLeaf* leaf = table_.chunk(leaf_index);
    const std::size_t faces_after = leaf->faces.size();
    if (leaf->node != 0xffffffffu) refit_ancestors(leaf->node);
    // Read what is needed BEFORE the split: creating a chunk may move the
    // table's records, so a pointer held across it is a pointer into the old
    // storage. Cheaper to take the two words than to re-look-up, and it says so
    // rather than depending on argument-evaluation order to be safe.
    if (faces_after > options_.max_leaf_faces) split_leaf(surface, leaf_index);
    // THE TREE IS NOT REBUILT HERE. `insert` runs once per face per topology
    // operation, and a stamp on a big surface runs thousands of them; rebuilding
    // the tree over the leaves inside it made the stamp O(operations x leaves)
    // and turned a 4x bigger model into a 40x slower dab. The rebuild is
    // deferred to the next QUERY, which needs it and happens once.
}

void DynamicBvh::split_leaf(const DynamicSurface& surface, std::uint32_t leaf_index) {
    const std::size_t count = table_.chunk(leaf_index)->faces.size();
    if (count < 2) return;

    // Median split on the widest axis, same rule as the build, with the same
    // slot tie-break so a leaf split during a stroke partitions the way a
    // rebuild would have.
    //
    // SORTED IN PLACE IN THE ARENA. The block is the chunk's own and nothing
    // else reads it while this runs, so the split costs no allocation at all —
    // which is what the adaptive half of the allocation gate asks for.
    math::Aabb spread;
    for (FaceId f : table_.chunk(leaf_index)->faces) spread.expand(centroid_of(surface, f));
    const kernel::cfloat3 ext = spread.extent();
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    FaceId* faces = table_.faces_mutable(leaf_index);
    std::sort(faces, faces + count, [&](FaceId a, FaceId b) {
        const kernel::cfloat3 ca = centroid_of(surface, a), cb = centroid_of(surface, b);
        const float va = axis == 0 ? ca.x : (axis == 1 ? ca.y : ca.z);
        const float vb = axis == 0 ? cb.x : (axis == 1 ? cb.y : cb.z);
        if (va != vb) return va < vb;
        return a.slot < b.slot;
    });

    // The second half is copied out before the new chunk is created, because
    // creating one may move the arena under the pointer the sort just used.
    const std::size_t mid = count / 2;
    moved_faces_.assign(faces + mid, faces + count);
    table_.truncate_faces(leaf_index, mid);

    const std::uint32_t other_index = table_.create();
    table_.assign_faces(other_index, moved_faces_.data(), moved_faces_.size());
    math::Aabb other_bounds;
    for (FaceId f : moved_faces_) {
        if (f.slot < face_leaf_.size()) face_leaf_[f.slot] = other_index;
        other_bounds.expand(face_bounds(surface, f));
    }
    table_.set_bounds(other_index, other_bounds);

    math::Aabb kept;
    for (FaceId f : table_.chunk(leaf_index)->faces) kept.expand(face_bounds(surface, f));
    table_.set_bounds(leaf_index, kept);

    mark_dirty(leaf_index, /*topology=*/true);
    mark_dirty(other_index, /*topology=*/true);
    // The leaf SET changed, so the tree over the leaves has to be rebuilt. That
    // is O(leaves), not O(faces), and it happens once per few hundred inserted
    // faces rather than per dab.
    tree_stale_ = true;
}

void DynamicBvh::erase(FaceId face) {
    if (face.slot >= face_leaf_.size()) return;
    const std::uint32_t leaf_index = face_leaf_[face.slot];
    if (leaf_index == kNoLeaf || table_.chunk(leaf_index) == nullptr) return;
    table_.remove_face(leaf_index, face.slot);
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
    const SurfaceLeaf* leaf = leaf_index == kNoLeaf ? nullptr : table_.chunk(leaf_index);
    if (leaf == nullptr) return;
    table_.expand_bounds(leaf_index, face_bounds(surface, face));
    mark_dirty(leaf_index, /*topology=*/false);
    if (leaf->node != 0xffffffffu) refit_ancestors(leaf->node);
}

void DynamicBvh::update_many(const DynamicSurface& surface, const std::vector<FaceId>& faces) {
    // One refit pass per LEAF rather than per face: a stamp usually touches a
    // few hundred faces in one or two leaves, and refitting the ancestors once
    // per face would multiply the logarithmic cost by the footprint.
    // The touched set is a member rather than a local: a stroke drains this
    // once per stamp and an allocation here would be one per dab.
    touched_.clear();
    for (FaceId f : faces) {
        if (f.slot >= face_leaf_.size()) continue;
        const std::uint32_t leaf_index = face_leaf_[f.slot];
        if (leaf_index == kNoLeaf || table_.chunk(leaf_index) == nullptr) continue;
        if (surface.live(f)) table_.expand_bounds(leaf_index, face_bounds(surface, f));
        mark_dirty(leaf_index, /*topology=*/false);
        touched_.push_back(leaf_index);
    }
    std::sort(touched_.begin(), touched_.end());
    touched_.erase(std::unique(touched_.begin(), touched_.end()), touched_.end());
    for (std::uint32_t i : touched_) {
        const SurfaceLeaf* leaf = table_.chunk(i);
        if (leaf->node != 0xffffffffu) refit_ancestors(leaf->node);
    }
}

// -- queries ------------------------------------------------------------------

void DynamicBvh::faces_in_ball(const DynamicSurface& surface, kernel::cfloat3 centre, float radius,
                               std::vector<FaceId>* out) const {
    out->clear();
    ensure_tree();
    if (root_ == 0xffffffffu || radius <= 0.0f) return;
    const float r2 = radius * radius;
    std::vector<std::uint32_t> stack{root_};
    while (!stack.empty()) {
        const std::uint32_t node = stack.back();
        stack.pop_back();
        const Node& n = nodes_[node];
        if (aabb_distance2(n.bounds, centre) > r2) continue;
        if (n.leaf == kNoLeaf) {
            if (n.left != 0xffffffffu) stack.push_back(n.left);
            if (n.right != 0xffffffffu) stack.push_back(n.right);
            continue;
        }
        // OVER-ADMITTED at the leaf, EXACT at the face: a leaf whose bounds
        // reach the ball may hold faces that do not.
        for (FaceId f : table_.chunk(n.leaf)->faces) {
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
    std::vector<std::uint32_t> stack{root_};
    while (!stack.empty()) {
        const std::uint32_t node = stack.back();
        stack.pop_back();
        const Node& n = nodes_[node];
        if (aabb_distance2(n.bounds, p) > best2) continue;
        if (n.leaf == kNoLeaf) {
            if (n.left != 0xffffffffu) stack.push_back(n.left);
            if (n.right != 0xffffffffu) stack.push_back(n.right);
            continue;
        }
        for (FaceId f : table_.chunk(n.leaf)->faces) {
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
    std::vector<std::uint32_t> stack{root_};
    while (!stack.empty()) {
        const std::uint32_t node = stack.back();
        stack.pop_back();
        const Node& n = nodes_[node];
        float enter = 0.0f;
        if (!aabb_hits_ray(n.bounds, origin, inv, &enter) || enter > best) continue;
        if (n.leaf == kNoLeaf) {
            if (n.left != 0xffffffffu) stack.push_back(n.left);
            if (n.right != 0xffffffffu) stack.push_back(n.right);
            continue;
        }
        for (FaceId f : table_.chunk(n.leaf)->faces) {
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
    // The SLOT count, which is the live count too: this partitioner never
    // releases a chunk, so the identity space stays dense and a caller
    // iterating 0..leaf_count() sees every leaf. A partitioner that did release
    // one would have to say so here rather than let the two quietly diverge.
    return table_.slot_count();
}

const SurfaceLeaf* DynamicBvh::leaf(std::uint32_t index) const { return table_.chunk(index); }

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
    if (table_.slot_count() == 0) return 1.0f;
    const math::Aabb whole = bounds();
    const kernel::cfloat3 we = whole.extent();
    const float total = std::max(we.x * we.y * we.z, 1e-20f);
    float sum = 0.0f;
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < table_.slot_count(); ++i) {
        const SurfaceLeaf* l = table_.chunk(i);
        if (l == nullptr || l->faces.empty()) continue;
        const kernel::cfloat3 e = l->bounds.extent();
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
    n += table_.bytes();
    n += face_leaf_.capacity() * sizeof(std::uint32_t);
    n += touched_.capacity() * sizeof(std::uint32_t) + moved_faces_.capacity() * sizeof(FaceId);
    return n;
}

}  // namespace mesh
}  // namespace clay
