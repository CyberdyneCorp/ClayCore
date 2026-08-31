#include "clay/mesh/adjacency.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>

namespace clay {
namespace mesh {
namespace {

struct CellKey {
    std::int64_t x = 0, y = 0, z = 0;
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellHash {
    std::size_t operator()(const CellKey& k) const {
        // The same mix VoxelCoordHash uses: three odd primes, so the lattice's
        // axis-aligned runs do not collide into one bucket.
        std::uint64_t h = static_cast<std::uint64_t>(k.x) * 0x9E3779B185EBCA87ull;
        h ^= static_cast<std::uint64_t>(k.y) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<std::uint64_t>(k.z) * 0x165667B19E3779F9ull;
        h ^= h >> 29;
        return static_cast<std::size_t>(h);
    }
};

// Exact-bit key, for weld_epsilon == 0. Comparing the BITS rather than the
// values is deliberate: -0.0f == 0.0f but they are different bytes, and a
// caller who asked for exact welding asked about the bytes.
struct BitKey {
    std::uint32_t x = 0, y = 0, z = 0;
    bool operator==(const BitKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct BitHash {
    std::size_t operator()(const BitKey& k) const {
        std::uint64_t h = k.x * 0x9E3779B1ull;
        h = (h ^ k.y) * 0xC2B2AE35ull;
        h = (h ^ k.z) * 0x27D4EB2Full;
        return static_cast<std::size_t>(h ^ (h >> 31));
    }
};

std::uint32_t bits_of(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

CellKey cell_of(kernel::cfloat3 p, float eps) {
    return CellKey{static_cast<std::int64_t>(std::floor(p.x / eps)),
                   static_cast<std::int64_t>(std::floor(p.y / eps)),
                   static_cast<std::int64_t>(std::floor(p.z / eps))};
}

float bbox_diagonal(const Mesh& m) {
    if (m.positions.empty()) return 0.0f;
    kernel::cfloat3 lo = m.positions[0], hi = m.positions[0];
    for (const kernel::cfloat3& p : m.positions) {
        lo = kernel::cmin(lo, p);
        hi = kernel::cmax(hi, p);
    }
    return kernel::clength(hi - lo);
}

// Class assignment by exact bit equality.
std::uint32_t weld_exact(const Mesh& m, std::vector<std::uint32_t>& class_of) {
    std::unordered_map<BitKey, std::uint32_t, BitHash> seen;
    seen.reserve(m.positions.size() * 2);
    std::uint32_t next = 0;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const kernel::cfloat3& p = m.positions[v];
        BitKey k{bits_of(p.x), bits_of(p.y), bits_of(p.z)};
        auto it = seen.find(k);
        if (it == seen.end()) {
            class_of[v] = next;
            seen.emplace(k, next);
            ++next;
        } else {
            class_of[v] = it->second;
        }
    }
    return next;
}

// Class assignment by proximity: a lattice of side `eps`, searching the 27
// cells around each vertex so a pair straddling a cell boundary still welds.
std::uint32_t weld_near(const Mesh& m, float eps, std::vector<std::uint32_t>& class_of) {
    std::unordered_map<CellKey, std::vector<std::uint32_t>, CellHash> buckets;
    buckets.reserve(m.positions.size());
    const float eps2 = eps * eps;
    std::uint32_t next = 0;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const kernel::cfloat3& p = m.positions[v];
        const CellKey home = cell_of(p, eps);
        std::uint32_t found = 0xffffffffu;
        for (int dz = -1; dz <= 1 && found == 0xffffffffu; ++dz)
            for (int dy = -1; dy <= 1 && found == 0xffffffffu; ++dy)
                for (int dx = -1; dx <= 1 && found == 0xffffffffu; ++dx) {
                    auto it = buckets.find(CellKey{home.x + dx, home.y + dy, home.z + dz});
                    if (it == buckets.end()) continue;
                    for (std::uint32_t r : it->second)
                        if (kernel::cdot2(m.positions[r] - p) <= eps2) {
                            found = class_of[r];
                            break;
                        }
                }
        if (found == 0xffffffffu) {
            class_of[v] = next++;
            buckets[home].push_back(static_cast<std::uint32_t>(v));
        } else {
            class_of[v] = found;
        }
    }
    return next;
}

// (key, value) pairs sorted, de-duplicated and turned into a CSR over
// `class_count` keys. One helper for both the ring and the triangle map: they
// differ only in what they collect.
void pairs_to_csr(std::vector<std::pair<std::uint32_t, std::uint32_t>>& pairs,
                  std::uint32_t class_count, std::vector<std::uint32_t>* offsets,
                  std::vector<std::uint32_t>* values) {
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    offsets->assign(class_count + 1, 0);
    for (const auto& kv : pairs) (*offsets)[kv.first + 1]++;
    for (std::uint32_t i = 0; i < class_count; ++i) (*offsets)[i + 1] += (*offsets)[i];
    values->resize(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i) (*values)[i] = pairs[i].second;
}

}  // namespace

Adjacency Adjacency::build(const Mesh& m, float weld_epsilon) {
    Adjacency a;
    a.triangle_count_ = m.triangle_count();
    a.class_of_.assign(m.positions.size(), 0);
    if (m.positions.empty()) {
        a.class_members_offsets_.assign(1, 0);
        a.ring_offsets_.assign(1, 0);
        a.tri_offsets_.assign(1, 0);
        return a;
    }

    const float eps = weld_epsilon > 0.0f ? weld_epsilon * bbox_diagonal(m) : 0.0f;
    const std::uint32_t classes =
        eps > 0.0f ? weld_near(m, eps, a.class_of_) : weld_exact(m, a.class_of_);

    // class -> members, by counting sort so the members of a class stay in
    // ascending vertex order.
    a.class_members_offsets_.assign(classes + 1, 0);
    for (std::uint32_t c : a.class_of_) a.class_members_offsets_[c + 1]++;
    for (std::uint32_t i = 0; i < classes; ++i)
        a.class_members_offsets_[i + 1] += a.class_members_offsets_[i];
    a.class_members_.resize(a.class_of_.size());
    {
        std::vector<std::uint32_t> cursor(a.class_members_offsets_.begin(),
                                          a.class_members_offsets_.end() - 1);
        for (std::size_t v = 0; v < a.class_of_.size(); ++v)
            a.class_members_[cursor[a.class_of_[v]]++] = static_cast<std::uint32_t>(v);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> ring_pairs, tri_pairs;
    ring_pairs.reserve(m.indices.size() * 2);
    tri_pairs.reserve(m.indices.size());
    const std::uint32_t vertex_count = static_cast<std::uint32_t>(a.class_of_.size());
    for (std::size_t t = 0; t < a.triangle_count_; ++t) {
        // AN OUT-OF-RANGE CORNER IS SKIPPED rather than followed. Nothing in
        // this library produces one and every entry point that takes indices
        // from outside checks them, so this is not a case anyone reaches by
        // accident — but a `Mesh` is a plain struct a C++ caller can fill by
        // hand, this function's contract never said the indices had to be in
        // range, and the consequence of taking one on trust is a read past the
        // end of `class_of_` rather than a wrong answer. A malformed triangle
        // contributes no ring and no incidence, which is the same treatment a
        // degenerate one already gets.
        if (m.indices[t * 3] >= vertex_count || m.indices[t * 3 + 1] >= vertex_count ||
            m.indices[t * 3 + 2] >= vertex_count)
            continue;
        const std::uint32_t c[3] = {a.class_of_[m.indices[t * 3]],
                                    a.class_of_[m.indices[t * 3 + 1]],
                                    a.class_of_[m.indices[t * 3 + 2]]};
        for (int i = 0; i < 3; ++i) {
            tri_pairs.emplace_back(c[i], static_cast<std::uint32_t>(t));
            const std::uint32_t j = c[(i + 1) % 3];
            if (c[i] == j) continue;  // a degenerate edge is not a neighbourhood
            ring_pairs.emplace_back(c[i], j);
            ring_pairs.emplace_back(j, c[i]);
        }
    }
    pairs_to_csr(ring_pairs, classes, &a.ring_offsets_, &a.ring_);
    pairs_to_csr(tri_pairs, classes, &a.tri_offsets_, &a.tris_);
    return a;
}

void geodesic_region(const Mesh& m, const Adjacency& adj, kernel::cfloat3 seed_position,
                     float radius, WalkScratch& scratch, std::vector<std::uint32_t>* out_classes,
                     std::vector<float>* out_distance, std::uint32_t seed_hint,
                     float path_budget_scale) {
    out_classes->clear();
    out_distance->clear();
    const std::uint32_t classes = static_cast<std::uint32_t>(adj.class_count());
    if (classes == 0 || radius <= 0.0f) return;

    // Class positions are read through the first member of each class: every
    // member holds the same position, that being what a class is.
    auto position_of = [&](std::uint32_t c) {
        std::size_t n = 0;
        return m.positions[adj.members(c, &n)[0]];
    };

    std::uint32_t seed = seed_hint;
    if (seed >= classes) {
        float best = std::numeric_limits<float>::max();
        seed = 0;
        for (std::uint32_t c = 0; c < classes; ++c) {
            const float d2 = kernel::cdot2(position_of(c) - seed_position);
            if (d2 < best) {
                best = d2;
                seed = c;
            }
        }
    }

    // The scratch is sized once and left dirty-free by whoever last used it,
    // so a stamp pays for the classes it REACHED rather than for the mesh.
    if (scratch.distance.size() != classes)
        scratch.distance.assign(classes, WalkScratch::kUnreached);
    scratch.dirty.clear();

    const float budget = radius * (path_budget_scale > 0.0f ? path_budget_scale : 1.0f);
    const float radius2 = radius * radius;

    // An explicit min-heap over the scratch's own buffer. `std::priority_queue`
    // is `push_heap`/`pop_heap` over a container it owns, so this is the same
    // algorithm producing the same pop sequence — and the region's ORDER is
    // load-bearing, because the weighted normal and centroid are float sums
    // over it and float addition is not associative.
    using Entry = std::pair<float, std::uint32_t>;
    const std::greater<Entry> later;
    std::vector<Entry>& frontier = scratch.frontier;
    frontier.clear();

    const float seed_d = kernel::clength(position_of(seed) - seed_position);
    if (seed_d > radius) return;
    scratch.distance[seed] = seed_d;
    scratch.dirty.push_back(seed);
    frontier.emplace_back(seed_d, seed);
    std::push_heap(frontier.begin(), frontier.end(), later);

    while (!frontier.empty()) {
        std::pop_heap(frontier.begin(), frontier.end(), later);
        const Entry top = frontier.back();
        frontier.pop_back();
        // A class can be pushed more than once; the first pop is its final
        // path length and the rest are stale.
        if (top.first > scratch.distance[top.second]) continue;
        out_classes->push_back(top.second);
        out_distance->push_back(top.first);

        const kernel::cfloat3 p = position_of(top.second);
        std::size_t n = 0;
        const std::uint32_t* ring = adj.ring(top.second, &n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t nb = ring[i];
            const kernel::cfloat3 q = position_of(nb);
            // (a) the path never leaves the ball...
            if (kernel::cdot2(q - seed_position) > radius2) continue;
            // ...and (b) it stays inside the budget.
            const float d = top.first + kernel::clength(q - p);
            if (d > budget) continue;
            const float known = scratch.distance[nb];
            if (known >= 0.0f && known <= d) continue;
            if (known < 0.0f) scratch.dirty.push_back(nb);
            scratch.distance[nb] = d;
            frontier.emplace_back(d, nb);
            std::push_heap(frontier.begin(), frontier.end(), later);
        }
    }
    for (std::uint32_t c : scratch.dirty) scratch.distance[c] = WalkScratch::kUnreached;
    scratch.dirty.clear();
}

void euclidean_region(const Mesh& m, const Adjacency& adj, kernel::cfloat3 centre, float radius,
                      std::vector<std::uint32_t>* out_classes, std::vector<float>* out_distance) {
    out_classes->clear();
    out_distance->clear();
    if (radius <= 0.0f) return;
    const float r2 = radius * radius;
    const std::uint32_t classes = static_cast<std::uint32_t>(adj.class_count());
    for (std::uint32_t c = 0; c < classes; ++c) {
        std::size_t n = 0;
        const kernel::cfloat3 p = m.positions[adj.members(c, &n)[0]];
        const float d2 = kernel::cdot2(p - centre);
        if (d2 > r2) continue;
        out_classes->push_back(c);
        out_distance->push_back(std::sqrt(d2));
    }
}

}  // namespace mesh
}  // namespace clay
