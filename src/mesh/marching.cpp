#include "clay/mesh/marching.h"

#include "clay/parallel/thread_pool.h"

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

// The same rule for a whole CELL rather than a triangle: the lowest requested
// key whose closed box holds one of the cell's eight corner lattice points.
//
// It exists because a cell owned by a brick with no lattice is marched by
// nobody else, so dropping its triangles is a hole rather than a subset
// boundary (issue #292) — and their crossing vertices sit in the OWNER's box,
// strictly outside every requested brick's, so the per-corner rule above would
// drop every one of them. The cell touched the request; that is what decides.
std::optional<brick::BrickKey> cell_attribution(int i, int j, int k, int dim,
                                                const RequestedSet& requested) {
    std::optional<brick::BrickKey> best;
    BrickKeyLess less;
    for (int c = 0; c < 8; ++c) {
        const std::array<float, 3> corner{static_cast<float>(i + (c & 1)),
                                          static_cast<float>(j + ((c >> 1) & 1)),
                                          static_cast<float>(k + ((c >> 2) & 1))};
        auto owner = lowest_requested_owner(corner, dim, requested);
        if (owner && (!best || less(*owner, *best))) best = owner;
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

// The bricks that can own a ring cell: a neighbour of some requested key that
// was not itself requested. Sorted by key, so the cells below come out the same
// way every call whatever order the caller's list or the cache's map was in.
std::vector<brick::BrickKey> ring_owners(const std::vector<brick::BrickKey>& keys,
                                         const RequestedSet& requested) {
    RequestedSet seen;
    std::vector<brick::BrickKey> owners;
    for (const brick::BrickKey& key : keys)
        for (int n = 0; n < 27; ++n) {
            if (n == 13) continue;  // the key itself
            const brick::BrickKey o{key.x + n % 3 - 1, key.y + n / 3 % 3 - 1, key.z + n / 9 - 1};
            if (!requested.count(o) && seen.insert(o).second) owners.push_back(o);
        }
    std::sort(owners.begin(), owners.end(), BrickKeyLess{});
    return owners;
}

// Whether any neighbour a cell reaches was requested. `wanted` is the owner's
// 3x3x3 neighbourhood flattened as (dz+1)*9 + (dy+1)*3 + dx+1; the three offset
// lists are what that cell's index reaches on each axis.
bool reaches_request(const bool wanted[27], const int* ax, int nx, const int* ay, int ny,
                     const int* az, int nz) {
    for (int a = 0; a < nz; ++a)
        for (int b = 0; b < ny; ++b)
            for (int c = 0; c < nx; ++c)
                if (wanted[(az[a] + 1) * 9 + (ay[b] + 1) * 3 + ax[c] + 1]) return true;
    return false;
}

// The cells `owner` owns that touch a requested brick, appended in z, y, x
// order.
void append_ring_cells(brick::BrickKey owner, int dim, const RequestedSet& requested,
                       std::vector<std::uint64_t>& out) {
    bool wanted[27];
    for (int n = 0; n < 27; ++n)
        wanted[n] = n != 13 && requested.count(brick::BrickKey{owner.x + n % 3 - 1,
                                                              owner.y + n / 3 % 3 - 1,
                                                              owner.z + n / 9 - 1});
    // Which neighbours one cell index reaches on its axis: always the owner
    // itself, plus the brick below when the index is on the owner's FIRST plane
    // — that brick's closed box includes it — and the brick above on its last.
    auto offsets = [dim](int c, int base, int* into) {
        int n = 0;
        if (c == base) into[n++] = -1;
        into[n++] = 0;
        if (c == base + dim - 1) into[n++] = 1;
        return n;
    };
    const int ox = owner.x * dim, oy = owner.y * dim, oz = owner.z * dim;
    int ax[3], ay[3], az[3];
    for (int k = oz; k < oz + dim; ++k) {
        const int nz = offsets(k, oz, az);
        for (int j = oy; j < oy + dim; ++j) {
            const int ny = offsets(j, oy, ay);
            // When y and z reach nobody, only x's two face planes can, so the
            // row's interior is stepped straight over rather than tested cell
            // by cell — most of a brick's cells are interior.
            const int step = (ny == 1 && nz == 1) ? std::max(dim - 1, 1) : 1;
            for (int i = ox; i < ox + dim; i += step) {
                const int nx = offsets(i, ox, ax);
                if (reaches_request(wanted, ax, nx, ay, ny, az, nz))
                    out.push_back(pack_point(i, j, k));
            }
        }
    }
}

// The ring cells to test: every cell whose closed span can touch a requested
// brick's closed box but is owned by an unrequested brick — one cell deep on
// every side, corners included.
//
// The owner's STATE is deliberately not consulted (issue #292). A cell is
// owned by the brick its low corner falls in, but the other seven corners
// belong to up to seven neighbours, so a cell owned by a brick that stores no
// lattice — uniform inside, uniform outside, or never evaluated — still
// crosses whenever one of those neighbours' samples has the opposite sign.
// That takes a field steeper than the band is wide, which is ordinary in a
// worked document: relief and incise displace by an amplitude over a region
// narrower than it, and the document says so through safe_step_scale. Skipping
// those owners left the crossing unmarched in BOTH paths and punched pinholes
// in the surface. They are marched here, and the whole-surface path collects
// them the same way, so the subset stays a filter of the whole.
//
// Walked one OWNER BRICK at a time rather than one key's ring at a time, and
// that is what keeps it affordable now that the whole-surface path runs it too.
// Ringing every key visits a shared cell from up to eight keys, so it needed a
// hash set to dedup and a sort to order — 42 ms of the pass's 51 on a
// 6,000-brick surface, against 9 ms for the marching it exists to feed. An
// owner owns each of its cells exactly once, so walking owners emits each cell
// once, with no set and no sort. The order — owners by key, then z, y, x within
// an owner — is fixed, which is all a reproducible replay needs.
std::vector<std::uint64_t> shell_cells(const std::vector<brick::BrickKey>& keys, int dim,
                                       const RequestedSet& requested) {
    std::vector<std::uint64_t> cells;
    for (const brick::BrickKey& owner : ring_owners(keys, requested))
        append_ring_cells(owner, dim, requested, cells);
    return cells;
}

// Every straddler a request owes, bucketed by the requested key each one is
// attributed to. Run for a subset AND for the whole surface: a subset owes the
// cells of the surface bricks it did not ask for, and both owe the cells of
// bricks that store no lattice at all (issue #292).
std::unordered_map<brick::BrickKey, std::vector<ShellTriangle>, brick::BrickKeyHash>
collect_straddlers(const brick::BrickCache& cache, const std::vector<brick::BrickKey>& keys,
                   const std::function<float(int, int, int)>& sample, int lod) {
    const int dim = cache.config().dim;
    RequestedSet requested(keys.begin(), keys.end());
    std::unordered_map<brick::BrickKey, std::vector<ShellTriangle>, brick::BrickKeyHash> buckets;
    constexpr std::uint64_t mask21 = (1u << 21) - 1;
    constexpr std::int64_t bias = 1u << 20;
    auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); };
    const std::vector<std::uint64_t> cells = shell_cells(keys, dim, requested);
    // MARCHED IN PARALLEL, BUCKETED SERIALLY — the same split, and for the same
    // reason, as the brick march below: `sample` is a const cache read and the
    // cells are independent, but the buckets are shared and their ORDER is what
    // makes a replay reproducible. So each cell records into its own collector
    // and the buckets are filled afterwards, walking `cells` in the order
    // shell_cells emitted them. Byte-identical to marching them one at a time.
    //
    // In waves, because a collector holds one entry per edge_vertex call and a
    // whole surface's ring is millions of cells; the wave bounds the recording
    // without changing the order it is consumed in.
    constexpr std::size_t kCellsPerWave = 8192;
    std::vector<ShellCollector> recorded;
    for (std::size_t wave = 0; wave < cells.size(); wave += kCellsPerWave) {
        const std::size_t wave_end = std::min(wave + kCellsPerWave, cells.size());
        const std::size_t wave_n = wave_end - wave;
        recorded.clear();
        recorded.resize(wave_n);
        parallel::for_range(wave_n, 1, [&](std::size_t first, std::size_t last) {
            for (std::size_t c = first; c < last; ++c) {
                const std::uint64_t packed = cells[wave + c];
                const int i = static_cast<int>(static_cast<std::int64_t>(packed >> 42) - bias);
                const int j =
                    static_cast<int>(static_cast<std::int64_t>((packed >> 21) & mask21) - bias);
                const int k = static_cast<int>(static_cast<std::int64_t>(packed & mask21) - bias);
                march_cell(recorded[c], sample, i, j, k);
            }
        });
        for (std::size_t c = 0; c < wave_n; ++c) {
            const ShellCollector& shell = recorded[c];
            if (shell.tris.empty()) continue;
            const std::uint64_t packed = cells[wave + c];
            const int i = static_cast<int>(static_cast<std::int64_t>(packed >> 42) - bias);
            const int j =
                static_cast<int>(static_cast<std::int64_t>((packed >> 21) & mask21) - bias);
            const int k = static_cast<int>(static_cast<std::int64_t>(packed & mask21) - bias);
            // Whether anyone ELSE marches this cell decides how it is
            // attributed. A cell owned by a surface brick is marched whenever
            // that brick is requested, so a subset takes only the triangles
            // that reach into it — the straddler rule. A cell owned by a brick
            // with no lattice at this level is marched by no request at all, so
            // it is kept whole.
            const brick::BrickKey owner{fdiv(i, dim), fdiv(j, dim), fdiv(k, dim)};
            const brick::Brick* stored = cache.find_lod(lod, owner);
            const bool owner_is_marched = stored && stored->state == brick::BrickState::Surface;
            std::optional<brick::BrickKey> whole_cell;
            if (!owner_is_marched) {
                whole_cell = cell_attribution(i, j, k, dim, requested);
                if (!whole_cell) continue;
            }
            for (const auto& tri : shell.tris) {
                auto attributed = owner_is_marched
                                      ? straddler_attribution(shell, tri, dim, requested)
                                      : whole_cell;
                if (!attributed) continue;
                buckets[*attributed].push_back(
                    {shell.edges[tri[0]], shell.edges[tri[1]], shell.edges[tri[2]]});
            }
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

namespace {

// The same lattice march, split across the pool — for a `sample` that is SAFE
// TO CALL CONCURRENTLY.
//
// Internal, and the public mesh_lattice above stays serial, for the reason
// FieldVolume::sample_parallel is a separate entry point: `sample` is a
// caller-supplied function and it may hold state. mesh_tape's is a pure array
// read of an already-evaluated grid, so it opts in; nothing else has to.
//
// THE SEAM WELD IS THE REPLAY, which is what makes this the same answer rather
// than a nearly-identical one. A slab records its triangles WITHOUT welding;
// the single Builder then re-emits every recorded edge through `edge_vertex`,
// which dedups exactly as it deduped the repeated calls the serial march made.
// Vertices shared across a slab boundary weld because the Builder sees both
// sides — the same mechanism parallel brick meshing (#111) uses, and the answer
// to the "vertex dedup across slab seams" #119 calls the fiddly one.
//
// Slabs are single Z planes replayed in Z order, and march_cells walks Z
// outermost, so the Builder sees exactly the call sequence the serial march
// made. Byte-identical by construction.
//
// In WAVES, so the recording is bounded by the wave rather than by the region:
// a fine mesh is millions of triangles and recording all of them before
// replaying any would trade time for a memory spike.
Mesh mesh_lattice_parallel(const std::function<float(int, int, int)>& sample,
                           const int cell_min[3], const int cell_max[3], kernel::cfloat3 origin,
                           float spacing) {
    Builder b(origin, spacing);
    const int z0 = cell_min[2], z1 = cell_max[2];
    // Below this the pool's dispatch costs more than the planes are worth.
    constexpr int kMinParallelPlanes = 8;
    if (z1 - z0 < kMinParallelPlanes) {
        march_cells(b, sample, cell_min, cell_max);
        return std::move(b.out);
    }

    constexpr int kPlanesPerWave = 64;
    std::vector<ShellCollector> recorded;
    std::vector<std::uint32_t> remap;
    for (int wave = z0; wave < z1; wave += kPlanesPerWave) {
        const int wave_end = std::min(wave + kPlanesPerWave, z1);
        const std::size_t n = static_cast<std::size_t>(wave_end - wave);
        recorded.clear();
        recorded.resize(n);
        parallel::for_range(n, 1, [&](std::size_t first, std::size_t last) {
            for (std::size_t s = first; s < last; ++s) {
                const int k = wave + static_cast<int>(s);
                const int cmin[3] = {cell_min[0], cell_min[1], k};
                const int cmax[3] = {cell_max[0], cell_max[1], k + 1};
                march_cells(recorded[s], sample, cmin, cmax);
            }
        });
        for (std::size_t s = 0; s < n; ++s) {
            const ShellCollector& rec = recorded[s];
            remap.assign(rec.edges.size(), 0);
            for (std::size_t v = 0; v < rec.edges.size(); ++v) {
                const ShellEdge& e = rec.edges[v];
                remap[v] = b.edge_vertex(e.p0, e.f0, e.p1, e.f1);
            }
            for (const std::array<std::uint32_t, 3>& tri : rec.tris)
                b.triangle(remap[tri[0]], remap[tri[1]], remap[tri[2]]);
        }
    }
    return std::move(b.out);
}

}  // namespace

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
    // Parallel: `sample` above is a pure read of the already-evaluated grid.
    Mesh m = mesh_lattice_parallel(sample, cmin, cmax, region.min, voxel_size);
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
    std::vector<brick::BrickKey> owned;
    if (!keys) {
        owned = cache.surface_bricks_lod(lod);
        keys = &owned;
    }
    if (out_ranges) {
        out_ranges->clear();
        out_ranges->reserve(keys->size());
    }

    // The straddlers: triangles from cells owned by a brick nobody asked for
    // that reach a corner into a requested one.
    //
    // The WHOLE-surface path owes them too (issue #292). It marches every cell
    // owned by a surface brick, and that is not every cell that crosses: a cell
    // takes seven of its eight corners from its neighbours, so one owned by a
    // brick with no lattice — uniform, or never evaluated — crosses as soon as
    // a neighbour's sample has the opposite sign, which a field steeper than
    // the band is wide does routinely. Those cells went unmarched and left
    // pinholes in the frame mesh. Collecting straddlers unconditionally marches
    // them once, attributed to the lowest requested key they touch; for a field
    // the band does bracket there are none and the mesh is unchanged.
    std::unordered_map<brick::BrickKey, std::vector<ShellTriangle>, brick::BrickKeyHash>
        straddlers = collect_straddlers(cache, *keys, global_sample, lod);

    // MARCH IN PARALLEL, WELD SERIALLY — and the split is forced by what makes
    // this mesh watertight rather than chosen for convenience.
    //
    // Marching a brick is pure: it reads the cache, which is a const lookup and
    // a half-to-float, and writes only its own output. Bricks are independent,
    // which is the premise of the sparse cache. So that half fans out.
    //
    // The welding cannot. ONE Builder serves every brick precisely so that a
    // lattice edge shared by two bricks yields ONE vertex — that is what keeps
    // the sparse set watertight at brick seams, and it means the vertex map is
    // shared mutable state that every brick touches. Sharding it per brick and
    // concatenating would duplicate every seam vertex and open the mesh along
    // every brick boundary.
    //
    // So phase one records what each brick WOULD emit, into the same
    // ShellCollector the straddler pass already uses for exactly this reason —
    // "no welding, since every recorded edge is re-emitted through the Builder
    // that welds" — and phase two replays those recordings through the single
    // Builder, in key order, calling edge_vertex in the same order the serial
    // loop called it.
    //
    // That last sentence is the correctness argument: the Builder sees an
    // identical call sequence, so it produces an identical vertex array, an
    // identical index array and identical ranges. Byte-identity with the serial
    // path is by construction rather than by tolerance, which is the house rule
    // for anything the pool touches.
    // Bound const so the parallel phase below cannot write to it, and so a
    // reader can see that it does not: every thread looks this map up and none
    // of them touches it.
    const auto& shared_straddlers = straddlers;
    // IN WAVES, because the recordings are transient memory that scales with
    // the model. Each brick's recorder holds one entry per edge_vertex call —
    // three per triangle, undeduplicated, since the welding it feeds is what
    // deduplicates — which measured ~40 KB per surface brick. Recording every
    // brick before welding any of them cost 94 MB on a 2,327-brick sphere and
    // would cost several hundred on a dense model, on a device that kills apps
    // for memory and whose brick budget is already a named concern.
    //
    // A wave is marched in parallel, welded, and its buffers reused. The weld
    // still walks keys in order across waves, so the builder sees the same call
    // sequence and the output is unchanged; only the peak is bounded. The wave
    // is large enough that the pool still has many chunks per worker to balance
    // across, so bounding the memory costs no parallelism.
    constexpr std::size_t kBricksPerWave = 512;
    std::vector<ShellCollector> recorded;
    Builder b(cf3(0, 0, 0), vs);
    std::vector<std::uint32_t> remap;
    for (std::size_t wave = 0; wave < keys->size(); wave += kBricksPerWave) {
    const std::size_t wave_end = std::min(wave + kBricksPerWave, keys->size());
    const std::size_t wave_n = wave_end - wave;
    recorded.clear();
    recorded.resize(wave_n);
    parallel::for_range(wave_n, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t w = first; w < last; ++w) {
            const std::size_t ki = wave + w;
            const brick::BrickKey& key = (*keys)[ki];
            ShellCollector& rec = recorded[w];
            int cmin[3] = {key.x * dim, key.y * dim, key.z * dim};
            int cmax[3] = {key.x * dim + dim, key.y * dim + dim, key.z * dim + dim};
            march_cells(rec, global_sample, cmin, cmax);
            // The key's straddlers are recorded after its own cells, which is
            // where the serial loop emitted them, so the replay below lands
            // them inside this key's range exactly as before.
            if (auto it = shared_straddlers.find(key); it != shared_straddlers.end())
                for (const ShellTriangle& tri : it->second) {
                    std::uint32_t v[3];
                    for (int c = 0; c < 3; ++c)
                        v[c] = rec.edge_vertex(tri[c].p0, tri[c].f0, tri[c].p1, tri[c].f1);
                    rec.triangle(v[0], v[1], v[2]);
                }
        }
    });

    for (std::size_t w = 0; w < wave_n; ++w) {
        const std::size_t ki = wave + w;
        const std::uint32_t v0 = static_cast<std::uint32_t>(b.out.positions.size());
        const std::uint32_t i0 = static_cast<std::uint32_t>(b.out.indices.size());
        const ShellCollector& rec = recorded[w];
        // ShellCollector does not weld, so one lattice edge used by several
        // tets appears several times here; the Builder dedups them exactly as
        // it deduped the repeated calls the serial march made.
        remap.assign(rec.edges.size(), 0);
        for (std::size_t v = 0; v < rec.edges.size(); ++v) {
            const ShellEdge& e = rec.edges[v];
            remap[v] = b.edge_vertex(e.p0, e.f0, e.p1, e.f1);
        }
        for (const std::array<std::uint32_t, 3>& tri : rec.tris)
            b.triangle(remap[tri[0]], remap[tri[1]], remap[tri[2]]);
        if (out_ranges)
            out_ranges->push_back({(*keys)[ki], v0,
                                   static_cast<std::uint32_t>(b.out.positions.size()) - v0, i0,
                                   static_cast<std::uint32_t>(b.out.indices.size()) - i0});
    }
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
