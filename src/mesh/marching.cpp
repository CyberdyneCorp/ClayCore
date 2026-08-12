#include "clay/mesh/marching.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <tuple>

#include <unordered_map>
#include <unordered_set>

#include "clay/eval/backend.h"
#include "clay/kernel/field.h"
#include "clay/scene/cull_index.h"

namespace clay {
namespace mesh {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// Freudenthal 6-tet decomposition, all tets sharing the 0-7 body diagonal
// (corner bit i: x=1, y=2, z=4). Every cube uses the same decomposition, so
// shared cube faces get identical diagonals -> consistent across cells.
constexpr int kTets[6][4] = {{0, 1, 5, 7}, {0, 5, 4, 7}, {0, 4, 6, 7},
                             {0, 6, 2, 7}, {0, 2, 3, 7}, {0, 3, 1, 7}};

struct LatticePoint {
    int i, j, k;
};

inline std::uint64_t pack_point(int i, int j, int k) {
    constexpr std::uint64_t bias = 1u << 20;
    return ((static_cast<std::uint64_t>(i) + bias) << 42) |
           ((static_cast<std::uint64_t>(j) + bias) << 21) |
           (static_cast<std::uint64_t>(k) + bias);
}

struct EdgeKey {
    std::uint64_t a, b;
    bool operator==(const EdgeKey&) const = default;
};
struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& e) const {
        return static_cast<std::size_t>(e.a * 0x9E3779B185EBCA87ull ^
                                        (e.b + 0xC2B2AE3D27D4EB4Full + (e.a << 6)));
    }
};

class Builder {
  public:
    Builder(kernel::cfloat3 origin, float spacing) : origin_(origin), spacing_(spacing) {}

    // Vertex on the crossing of lattice edge (p0, p1); welded by canonical key.
    std::uint32_t edge_vertex(LatticePoint p0, float f0, LatticePoint p1, float f1) {
        std::uint64_t id0 = pack_point(p0.i, p0.j, p0.k);
        std::uint64_t id1 = pack_point(p1.i, p1.j, p1.k);
        if (id0 > id1) {
            std::swap(id0, id1);
            std::swap(p0, p1);
            std::swap(f0, f1);
        }
        EdgeKey key{id0, id1};
        auto it = vertex_map_.find(key);
        if (it != vertex_map_.end()) return it->second;
        float t = f0 / (f0 - f1);  // f0, f1 have opposite signs
        cfloat3 a = origin_ + cf3((float)p0.i, (float)p0.j, (float)p0.k) * spacing_;
        cfloat3 b = origin_ + cf3((float)p1.i, (float)p1.j, (float)p1.k) * spacing_;
        std::uint32_t idx = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(a + (b - a) * t);
        vertex_map_.emplace(key, idx);
        return idx;
    }

    void triangle(std::uint32_t v0, std::uint32_t v1, std::uint32_t v2) {
        out.indices.push_back(v0);
        out.indices.push_back(v1);
        out.indices.push_back(v2);
    }

    Mesh out;

  private:
    kernel::cfloat3 origin_;
    float spacing_;
    std::unordered_map<EdgeKey, std::uint32_t, EdgeKeyHash> vertex_map_;
};

// March one tetrahedron with exact combinatorial winding.
//
// For a POSITIVELY oriented tet and inside vertex at slot i, the crossing
// triangle (e_ij, e_ik, e_il) is outward-wound, where (j,k,l) makes
// (i,j,k,l) an even permutation of (0,1,2,3). All six Freudenthal tets in
// kTets are NEGATIVELY oriented (det = -1, verified below in a static
// assert-style comment), so every rule is mirrored once. This is robust for
// sliver triangles where geometric normal checks break down.
//
//   dets of kTets vs unit cube corners: all -1.
constexpr int kEvenRest[4][3] = {{1, 2, 3}, {0, 3, 2}, {3, 0, 1}, {2, 1, 0}};
// inside-pair -> even permutation (p, q, r, s)
constexpr int kPairPerm[6][4] = {{0, 1, 2, 3}, {0, 2, 3, 1}, {0, 3, 1, 2},
                                 {1, 2, 0, 3}, {1, 3, 2, 0}, {2, 3, 0, 1}};

// Sink is Builder, or any type with the same edge_vertex/triangle shape —
// ShellCollector below records triangles instead of building a mesh.
template <class Sink>
void march_tet(Sink& b, const LatticePoint corners[4], const float f[4]) {
    // inside = strictly negative; f == 0 counts as outside (deterministic)
    int inside_mask = 0;
    for (int i = 0; i < 4; ++i)
        if (f[i] < 0.0f) inside_mask |= 1 << i;
    if (inside_mask == 0 || inside_mask == 0xF) return;

    auto ev = [&](int a2, int b2) {
        return b.edge_vertex(corners[a2], f[a2], corners[b2], f[b2]);
    };
    auto emit = [&](std::uint32_t v0, std::uint32_t v1, std::uint32_t v2) {
        b.triangle(v0, v1, v2);
    };

    int ni = 0;
    for (int i = 0; i < 4; ++i)
        if (f[i] < 0.0f) ++ni;

    if (ni == 1) {
        int i = 0;
        while (!(inside_mask & (1 << i))) ++i;
        const int* r = kEvenRest[i];
        // det>0 rule (e_ij, e_ik, e_il), mirrored once for det<0 tets
        emit(ev(i, r[0]), ev(i, r[2]), ev(i, r[1]));
    } else if (ni == 3) {
        int i = 0;
        while (inside_mask & (1 << i)) ++i;  // the single outside slot
        const int* r = kEvenRest[i];
        emit(ev(i, r[0]), ev(i, r[1]), ev(i, r[2]));
    } else {
        const int* pp = nullptr;
        for (const auto& cand : kPairPerm) {
            int mask = (1 << cand[0]) | (1 << cand[1]);
            if (mask == inside_mask) {
                pp = cand;
                break;
            }
        }
        int p = pp[0], q = pp[1], r2 = pp[2], s2 = pp[3];
        std::uint32_t epr = ev(p, r2), eps = ev(p, s2), eqs = ev(q, s2), eqr = ev(q, r2);
        // det>0: (0,1,2)+(0,2,3) of quad (epr,eps,eqs,eqr); mirrored:
        emit(epr, eqs, eps);
        emit(epr, eqr, eqs);
    }
}

template <class Sink>
void march_cell(Sink& b, const std::function<float(int, int, int)>& sample, int i, int j, int k) {
    LatticePoint pts[8];
    float f[8];
    bool any_neg = false, any_pos = false;
    for (int c = 0; c < 8; ++c) {
        pts[c] = {i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1)};
        f[c] = sample(pts[c].i, pts[c].j, pts[c].k);
        (f[c] < 0.0f ? any_neg : any_pos) = true;
    }
    if (!any_neg || !any_pos) return;
    for (const auto& tet : kTets) {
        LatticePoint tc[4] = {pts[tet[0]], pts[tet[1]], pts[tet[2]], pts[tet[3]]};
        float tf[4] = {f[tet[0]], f[tet[1]], f[tet[2]], f[tet[3]]};
        march_tet(b, tc, tf);
    }
}

template <class Sink>
void march_cells(Sink& b, const std::function<float(int, int, int)>& sample,
                 const int cell_min[3], const int cell_max[3]) {
    for (int k = cell_min[2]; k < cell_max[2]; ++k)
        for (int j = cell_min[1]; j < cell_max[1]; ++j)
            for (int i = cell_min[0]; i < cell_max[0]; ++i) march_cell(b, sample, i, j, k);
}

// -- subset boundary straddlers ---------------------------------------------
//
// A subset request marches the cells its keys OWN, and cell ownership puts
// every triangle in exactly one brick. But a triangle produced by a cell just
// outside the subset can still have a corner inside a requested brick — the
// straddlers of the meshing spec — and without them no sequence of subset
// meshes can maintain a complete surface (issue #66). The shell pass below
// marches the one-cell ring of cells owned by unrequested SURFACE bricks and
// keeps the triangles that reach into the request, attributing each to the
// lexicographically lowest (x, then y, then z) requested key whose closed
// brick box contains one of its corners.

// One lattice-edge crossing as march_tet found it: enough to re-emit the
// vertex through the real Builder later, with identical welding and an
// identical position.
struct ShellEdge {
    LatticePoint p0, p1;
    float f0 = 0.0f, f1 = 0.0f;
};
using ShellTriangle = std::array<ShellEdge, 3>;

// Records triangles instead of building a mesh; no welding, since every
// recorded edge is re-emitted through the Builder that welds.
struct ShellCollector {
    std::vector<ShellEdge> edges;
    std::vector<std::array<std::uint32_t, 3>> tris;

    std::uint32_t edge_vertex(LatticePoint p0, float f0, LatticePoint p1, float f1) {
        edges.push_back({p0, p1, f0, f1});
        return static_cast<std::uint32_t>(edges.size() - 1);
    }
    void triangle(std::uint32_t v0, std::uint32_t v1, std::uint32_t v2) {
        tris.push_back({v0, v1, v2});
    }
};

struct BrickKeyLess {
    bool operator()(const brick::BrickKey& a, const brick::BrickKey& b) const {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    }
};

using RequestedSet = std::unordered_set<brick::BrickKey, brick::BrickKeyHash>;

// The corner's position on its lattice edge, in lattice units, computed with
// exactly the arithmetic Builder::edge_vertex uses (canonical endpoint order,
// then t = f0 / (f0 - f1)) so classification agrees with the emitted vertex.
std::array<float, 3> shell_corner_lattice(ShellEdge e) {
    if (pack_point(e.p0.i, e.p0.j, e.p0.k) > pack_point(e.p1.i, e.p1.j, e.p1.k)) {
        std::swap(e.p0, e.p1);
        std::swap(e.f0, e.f1);
    }
    const float t = e.f0 / (e.f0 - e.f1);
    return {static_cast<float>(e.p0.i) + (static_cast<float>(e.p1.i - e.p0.i)) * t,
            static_cast<float>(e.p0.j) + (static_cast<float>(e.p1.j - e.p0.j)) * t,
            static_cast<float>(e.p0.k) + (static_cast<float>(e.p1.k - e.p0.k)) * t};
}

// The lowest requested key whose CLOSED brick box contains the corner, if any.
// A coordinate exactly on a brick boundary plane belongs to both neighbours,
// so each axis can name two candidate brick indices.
std::optional<brick::BrickKey> lowest_requested_owner(const std::array<float, 3>& c, int dim,
                                                      const RequestedSet& requested) {
    int lo[3], n[3][2];
    for (int a = 0; a < 3; ++a) {
        const float b = c[a] / static_cast<float>(dim);
        const int fb = static_cast<int>(std::floor(b));
        n[a][0] = fb;
        n[a][1] = fb;
        lo[a] = 1;
        if (b == static_cast<float>(fb)) {  // on the boundary plane: both bricks
            n[a][1] = fb - 1;
            lo[a] = 2;
        }
    }
    std::optional<brick::BrickKey> best;
    BrickKeyLess less;
    for (int x = 0; x < lo[0]; ++x)
        for (int y = 0; y < lo[1]; ++y)
            for (int z = 0; z < lo[2]; ++z) {
                brick::BrickKey k{n[0][x], n[1][y], n[2][z]};
                if (!requested.count(k)) continue;
                if (!best || less(k, *best)) best = k;
            }
    return best;
}

std::optional<brick::BrickKey> straddler_attribution(const ShellCollector& shell,
                                                    const std::array<std::uint32_t, 3>& tri,
                                                    int dim, const RequestedSet& requested) {
    std::optional<brick::BrickKey> best;
    BrickKeyLess less;
    for (std::uint32_t corner : tri) {
        auto owner =
            lowest_requested_owner(shell_corner_lattice(shell.edges[corner]), dim, requested);
        if (owner && (!best || less(*owner, *best))) best = owner;
    }
    return best;
}

// The ring cells to test: every cell whose closed span can touch a requested
// brick's closed box but is owned by an unrequested brick — one cell deep on
// every side, corners included. Owners that are not surface bricks are
// skipped: the whole-surface mesh marches no cell of theirs, and the subset
// must stay a filter of the whole, never a superset.
std::vector<std::uint64_t> shell_cells(const brick::BrickCache& cache,
                                       const std::vector<brick::BrickKey>& keys, int dim,
                                       const RequestedSet& requested, int lod) {
    auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); };
    std::unordered_set<std::uint64_t> seen;
    std::vector<std::uint64_t> cells;
    for (const brick::BrickKey& key : keys) {
        for (int k = key.z * dim - 1; k <= key.z * dim + dim; ++k)
            for (int j = key.y * dim - 1; j <= key.y * dim + dim; ++j)
                for (int i = key.x * dim - 1; i <= key.x * dim + dim; ++i) {
                    const bool ring = i < key.x * dim || i >= key.x * dim + dim ||
                                      j < key.y * dim || j >= key.y * dim + dim ||
                                      k < key.z * dim || k >= key.z * dim + dim;
                    if (!ring) continue;
                    brick::BrickKey owner{fdiv(i, dim), fdiv(j, dim), fdiv(k, dim)};
                    if (requested.count(owner)) continue;
                    // The level's own bricks, so the subset stays a filter of
                    // the whole mesh AT THAT LEVEL rather than borrowing cells
                    // from another one.
                    const brick::Brick* b = cache.find_lod(lod, owner);
                    if (!b || b->state != brick::BrickState::Surface) continue;
                    if (seen.insert(pack_point(i, j, k)).second)
                        cells.push_back(pack_point(i, j, k));
                }
    }
    // pack_point orders lexicographically by (i, j, k): a deterministic march
    // order, so a bucket's triangles come out the same way every call.
    std::sort(cells.begin(), cells.end());
    return cells;
}

// Every straddler a subset request owes, bucketed by the requested key each
// one is attributed to.
std::unordered_map<brick::BrickKey, std::vector<ShellTriangle>, brick::BrickKeyHash>
collect_straddlers(const brick::BrickCache& cache, const std::vector<brick::BrickKey>& keys,
                   const std::function<float(int, int, int)>& sample, int lod) {
    const int dim = cache.config().dim;
    RequestedSet requested(keys.begin(), keys.end());
    std::unordered_map<brick::BrickKey, std::vector<ShellTriangle>, brick::BrickKeyHash> buckets;
    constexpr std::uint64_t mask21 = (1u << 21) - 1;
    constexpr std::int64_t bias = 1u << 20;
    for (std::uint64_t packed : shell_cells(cache, keys, dim, requested, lod)) {
        const int i = static_cast<int>(static_cast<std::int64_t>(packed >> 42) - bias);
        const int j = static_cast<int>(static_cast<std::int64_t>((packed >> 21) & mask21) - bias);
        const int k = static_cast<int>(static_cast<std::int64_t>(packed & mask21) - bias);
        ShellCollector shell;
        march_cell(shell, sample, i, j, k);
        for (const auto& tri : shell.tris) {
            auto owner = straddler_attribution(shell, tri, dim, requested);
            if (!owner) continue;
            buckets[*owner].push_back(
                {shell.edges[tri[0]], shell.edges[tri[1]], shell.edges[tri[2]]});
        }
    }
    return buckets;
}

}  // namespace

Mesh mesh_lattice(const std::function<float(int, int, int)>& sample, int cell_min[3],
                  int cell_max[3], kernel::cfloat3 origin, float spacing) {
    Builder b(origin, spacing);
    march_cells(b, sample, cell_min, cell_max);
    return std::move(b.out);
}

Mesh mesh_tape(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
               const MeshingOptions& options) {
    if (region.empty() || region.is_infinite()) return {};
    // The lattice below is sized from voxel_size, so it is the caller's number
    // that decides the allocation. Priced in double before anything is cast to
    // int: a size fine enough to overflow the int conversion is undefined, and
    // one merely enormous ends the process in the allocator.
    if (!(voxel_size > 0.0f) || !std::isfinite(voxel_size)) return {};
    const double dx = static_cast<double>(region.max.x - region.min.x) / voxel_size + 2.0;
    const double dy = static_cast<double>(region.max.y - region.min.y) / voxel_size + 2.0;
    const double dz = static_cast<double>(region.max.z - region.min.z) / voxel_size + 2.0;
    if (!(dx * dy * dz <= static_cast<double>(kMaxGridSamples))) return {};

    int nx = static_cast<int>(kernel::cround((region.max.x - region.min.x) / voxel_size)) + 1;
    int ny = static_cast<int>(kernel::cround((region.max.y - region.min.y) / voxel_size)) + 1;
    int nz = static_cast<int>(kernel::cround((region.max.z - region.min.z) / voxel_size)) + 1;

    eval::GridQuery grid;
    grid.origin = region.min;
    grid.spacing = voxel_size;
    grid.nx = nx;
    grid.ny = ny;
    grid.nz = nz;
    std::vector<float> values(static_cast<std::size_t>(nx) * ny * nz);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    if (!cpu || cpu->eval_grid(tape, grid, values.data()) != eval::Status::Ok) return {};

    auto sample = [&](int i, int j, int k) -> float {
        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz)
            return kernel::cabs(voxel_size);  // outside the region
        return values[(static_cast<std::size_t>(k) * ny + j) * nx + i];
    };
    // march one extra ring so geometry crossing the region boundary is
    // CLOSED against the positive out-of-range samples (stays watertight)
    int cmin[3] = {-1, -1, -1};
    int cmax[3] = {nx, ny, nz};
    Mesh m = mesh_lattice(sample, cmin, cmax, region.min, voxel_size);
    apply_tape_attributes(m, tape, options);
    return m;
}

namespace {

// Field attributes for a brick mesh, through PER-BRICK culled tapes rather
// than the whole document's (issue #73): with the full tape every vertex paid
// O(document), so re-meshing a FIXED brick set grew with everything already
// sculpted — 4.8 ms at 1 node to 120 ms at 193 in the report — while the
// refill path over the same bricks stayed flat, because it culls. Vertices
// are grouped by the brick that owns their position and each group is
// evaluated against a tape culled to that brick's band-dilated region, the
// region BrickCache::cull_region defines and refill culls against. Inside it
// band-clamped results are bit-identical to the full tape's; the vertices sit
// on the surface (|d| ~ 0) and gradient taps move gradient_eps << band, so
// every sample lands where the tapes agree exactly and the attributes match a
// full-tape evaluation bit for bit.
void apply_brick_attributes(Mesh& m, const brick::BrickCache& cache, const scene::Document& doc,
                            const MeshingOptions& options,
                            const scene::CullIndex* cull_index) {
    const bool colors = options.colors;
    const bool gradients = options.normals == NormalMode::Gradient;
    const float brick_width =
        static_cast<float>(cache.config().dim) * cache.config().voxel_size;
    std::unordered_map<brick::BrickKey, std::vector<std::uint32_t>, brick::BrickKeyHash> groups;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const cfloat3 p = m.positions[i];
        // A vertex exactly on a boundary plane floors into its upper
        // neighbour, whose cull region still contains it: the region is the
        // brick dilated by the band, and the band is >= 1 voxel.
        const brick::BrickKey key{static_cast<int>(std::floor(p.x / brick_width)),
                                  static_cast<int>(std::floor(p.y / brick_width)),
                                  static_cast<int>(std::floor(p.z / brick_width))};
        groups[key].push_back(static_cast<std::uint32_t>(i));
    }
    if (colors) m.colors.resize(m.positions.size());
    if (gradients) m.normals.resize(m.positions.size());
    // One cull index and one coarse plan for the whole vertex set (an
    // uncached index costs one bounds pass — what a single per-brick compile
    // used to pay — amortized over every group): each group's compile then
    // walks only the items near the meshed bricks, not the whole document.
    std::optional<scene::CullIndex> local_index;
    if (!cull_index) {
        local_index.emplace(doc);
        cull_index = &*local_index;
    }
    math::Aabb all_regions;
    for (const auto& [key, verts] : groups)
        all_regions.expand(cache.cull_region(key).dilated(options.gradient_eps));
    const scene::CullPlan plan = cull_index->plan(all_regions);
    // ONE culled tape per group, shared by the colour and the normal of every
    // vertex the group holds. The evaluation goes to the CPU backend as a
    // single flattened batch (eval_points_batch) rather than a serial loop
    // here: with the index and plan the compiles above are cheap, and what
    // kept a dense re-mesh slow was five scalar field taps per vertex on one
    // core while the refill over the same bricks ran on all of them. The
    // backend's reference path computes exactly the taps this loop computed —
    // same tape, same points, same gradient_eps — so the attributes are
    // unchanged bit for bit.
    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> tape_ptrs;
    std::vector<std::size_t> offsets;
    std::vector<std::uint32_t> order;  // vertex index per flattened point
    tapes.reserve(groups.size());
    tape_ptrs.reserve(groups.size());
    offsets.reserve(groups.size() + 1);
    order.reserve(m.positions.size());
    offsets.push_back(0);
    for (const auto& [key, verts] : groups) {
        // Dilated by gradient_eps on top of the band, so the tetrahedron taps
        // of a vertex at the region's edge stay inside the culled zone.
        const scene::CullRegion cull{cache.cull_region(key).dilated(options.gradient_eps)};
        tapes.push_back(scene::compile_document(doc, &cull, cull_index, &plan));
        order.insert(order.end(), verts.begin(), verts.end());
        offsets.push_back(order.size());
    }
    for (const scene::Tape& tape : tapes) tape_ptrs.push_back(&tape);
    std::vector<float> points(order.size() * 3);
    for (std::size_t at = 0; at < order.size(); ++at) {
        const cfloat3 p = m.positions[order[at]];
        points[at * 3] = p.x;
        points[at * 3 + 1] = p.y;
        points[at * 3 + 2] = p.z;
    }
    std::vector<float> distances(order.size());
    std::vector<float> grads(gradients ? order.size() * 3 : 0);
    std::vector<float> cols(colors ? order.size() * 3 : 0);
    eval::PointBatchQuery q;
    q.tapes = tape_ptrs.data();
    q.offsets = offsets.data();
    q.points_xyz = points.data();
    q.count = tapes.size();
    q.gradient_eps = options.gradient_eps;
    eval::PointResults out;
    out.distances = distances.data();
    out.gradients_xyz = gradients ? grads.data() : nullptr;
    out.colors_rgb = colors ? cols.data() : nullptr;
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    if (cpu && cpu->eval_points_batch(q, out) == eval::Status::Ok) {
        for (std::size_t at = 0; at < order.size(); ++at) {
            const std::uint32_t i = order[at];
            if (colors) m.colors[i] = cf3(cols[at * 3], cols[at * 3 + 1], cols[at * 3 + 2]);
            if (gradients)
                m.normals[i] = cf3(grads[at * 3], grads[at * 3 + 1], grads[at * 3 + 2]);
        }
        return;
    }
    // No CPU backend registered: the same evaluation, serially.
    for (std::size_t g = 0; g < tapes.size(); ++g) {
        const scene::Tape& tape = tapes[g];
        auto field = [&](cfloat3 p) { return tape.eval(p).d; };
        for (std::size_t at = offsets[g]; at < offsets[g + 1]; ++at) {
            const std::uint32_t i = order[at];
            if (colors) m.colors[i] = tape.eval(m.positions[i]).color;
            if (gradients)
                m.normals[i] = kernel::cnormal(field, m.positions[i], options.gradient_eps);
        }
    }
}

}  // namespace

Mesh mesh_bricks(const brick::BrickCache& cache, const scene::Document* doc_for_attributes,
                 const MeshingOptions& options, const std::vector<brick::BrickKey>* keys,
                 std::vector<BrickMeshRange>* out_ranges, const scene::CullIndex* cull_index,
                 int lod) {
    // There is one mip level. A level the cache cannot hold has no bricks and
    // no lattice, so it meshes to nothing rather than to whichever level is
    // nearest — answering level 0 for a request for level 4 would put geometry
    // at the wrong size on screen, which is the reason read_bricks rejects the
    // same request.
    if (lod < 0 || lod > 1) {
        if (out_ranges) out_ranges->clear();
        return {};
    }
    const int dim = cache.config().dim;
    // A mip keeps every second lattice point of the block it covers, so its
    // lattice is the cache's own at twice the spacing and still anchored at the
    // world origin: doubling the spacing per level is the whole of the
    // transform, and the marcher needs nothing else.
    const float vs = cache.config().voxel_size * static_cast<float>(1 << lod);
    auto global_sample = [&](int i, int j, int k) -> float {
        auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); };
        brick::BrickKey key{fdiv(i, dim), fdiv(j, dim), fdiv(k, dim)};
        return cache.sample_lod(lod, key, i - key.x * dim, j - key.y * dim, k - key.z * dim);
    };

    // The subset a consumer named, or every brick the LEVEL stores — which is
    // the export path and stays the default.
    const bool is_subset = keys != nullptr;
    std::vector<brick::BrickKey> owned;
    if (!keys) {
        owned = cache.surface_bricks_lod(lod);
        keys = &owned;
    }
    if (out_ranges) {
        out_ranges->clear();
        out_ranges->reserve(keys->size());
    }

    // A subset also owes the straddlers: triangles from cells owned by
    // unrequested surface bricks that reach a corner into a requested brick.
    // The whole-surface path marches every cell already and owes nothing.
    std::unordered_map<brick::BrickKey, std::vector<ShellTriangle>, brick::BrickKeyHash>
        straddlers;
    if (is_subset) straddlers = collect_straddlers(cache, *keys, global_sample, lod);

    // ONE builder for every brick: lattice-edge welding spans brick seams,
    // keeping the result watertight across the sparse set.
    Builder b(cf3(0, 0, 0), vs);
    for (const brick::BrickKey& key : *keys) {
        const std::uint32_t v0 = static_cast<std::uint32_t>(b.out.positions.size());
        const std::uint32_t i0 = static_cast<std::uint32_t>(b.out.indices.size());
        int cmin[3] = {key.x * dim, key.y * dim, key.z * dim};
        int cmax[3] = {key.x * dim + dim, key.y * dim + dim, key.z * dim + dim};
        march_cells(b, global_sample, cmin, cmax);
        // The key's straddlers land inside its range, after its own cells, so
        // the ranges keep partitioning the mesh. Re-emitting through the same
        // Builder welds them onto the seam vertices the interior already made.
        if (auto it = straddlers.find(key); it != straddlers.end())
            for (const ShellTriangle& tri : it->second) {
                std::uint32_t v[3];
                for (int c = 0; c < 3; ++c)
                    v[c] = b.edge_vertex(tri[c].p0, tri[c].f0, tri[c].p1, tri[c].f1);
                b.triangle(v[0], v[1], v[2]);
            }
        if (out_ranges)
            out_ranges->push_back({key, v0,
                                   static_cast<std::uint32_t>(b.out.positions.size()) - v0, i0,
                                   static_cast<std::uint32_t>(b.out.indices.size()) - i0});
    }
    Mesh m = std::move(b.out);
    // Level 0 only: the per-brick culled tapes are bit-identical to the full
    // document's for a vertex on the FIELD's surface, and a coarse vertex sits
    // on the mip's instead — up to most of a coarse cell away, where the two
    // tapes are only both-outside-the-band rather than equal. The C boundary
    // refuses the request; in-engine it is skipped, and face normals below
    // still answer because they need no field.
    if (lod == 0 && doc_for_attributes &&
        (options.colors || options.normals == NormalMode::Gradient))
        apply_brick_attributes(m, cache, *doc_for_attributes, options, cull_index);
    // Face normals are area-weighted from the triangles and need no field,
    // which is what NormalMode::Face means and what the C header promises a
    // caller who passes no document ("positions and face normals"). Without
    // this the promise was silently broken: attributes were applied only
    // through the tape, so a document-less brick mesh came back with no
    // normals at all and a host shaded it flat black.
    if (options.normals == NormalMode::Face) compute_face_normals(m);
    return m;
}

void apply_tape_attributes(Mesh& m, const scene::Tape& tape, const MeshingOptions& options) {
    if (options.colors) {
        m.colors.resize(m.positions.size());
        for (std::size_t i = 0; i < m.positions.size(); ++i)
            m.colors[i] = tape.eval(m.positions[i]).color;
    }
    if (options.normals == NormalMode::Gradient) {
        m.normals.resize(m.positions.size());
        auto field = [&](cfloat3 p) { return tape.eval(p).d; };
        for (std::size_t i = 0; i < m.positions.size(); ++i)
            m.normals[i] = kernel::cnormal(field, m.positions[i], options.gradient_eps);
    } else if (options.normals == NormalMode::Face) {
        compute_face_normals(m);
    }
}

void compute_face_normals(Mesh& m) {
    m.normals.assign(m.positions.size(), cf3(0, 0, 0));
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        std::uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        cfloat3 n = kernel::ccross(m.positions[i1] - m.positions[i0],
                                   m.positions[i2] - m.positions[i0]);
        m.normals[i0] = m.normals[i0] + n;  // area-weighted
        m.normals[i1] = m.normals[i1] + n;
        m.normals[i2] = m.normals[i2] + n;
    }
    for (cfloat3& n : m.normals) {
        float len = kernel::clength(n);
        n = len > 1e-20f ? n / len : cf3(0, 1, 0);
    }
}

void uv_box_project(Mesh& m, float scale) {
    if (m.normals.size() != m.positions.size()) compute_face_normals(m);
    m.uvs.resize(m.positions.size());
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        cfloat3 n = kernel::cabs(m.normals[i]);
        cfloat3 p = m.positions[i] / scale;
        if (n.x >= n.y && n.x >= n.z)
            m.uvs[i] = kernel::cf2(p.y, p.z);
        else if (n.y >= n.z)
            m.uvs[i] = kernel::cf2(p.x, p.z);
        else
            m.uvs[i] = kernel::cf2(p.x, p.y);
    }
}

}  // namespace mesh
}  // namespace clay
