#include "clay/mesh/marching.h"

#include <cmath>

#include <unordered_map>

#include "clay/eval/backend.h"
#include "clay/kernel/field.h"

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

void march_tet(Builder& b, const LatticePoint corners[4], const float f[4]) {
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

void march_cells(Builder& b, const std::function<float(int, int, int)>& sample,
                 const int cell_min[3], const int cell_max[3]) {
    for (int k = cell_min[2]; k < cell_max[2]; ++k)
        for (int j = cell_min[1]; j < cell_max[1]; ++j)
            for (int i = cell_min[0]; i < cell_max[0]; ++i) {
                LatticePoint pts[8];
                float f[8];
                bool any_neg = false, any_pos = false;
                for (int c = 0; c < 8; ++c) {
                    pts[c] = {i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1)};
                    f[c] = sample(pts[c].i, pts[c].j, pts[c].k);
                    (f[c] < 0.0f ? any_neg : any_pos) = true;
                }
                if (!any_neg || !any_pos) continue;
                for (const auto& tet : kTets) {
                    LatticePoint tc[4] = {pts[tet[0]], pts[tet[1]], pts[tet[2]], pts[tet[3]]};
                    float tf[4] = {f[tet[0]], f[tet[1]], f[tet[2]], f[tet[3]]};
                    march_tet(b, tc, tf);
                }
            }
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

Mesh mesh_bricks(const brick::BrickCache& cache, const scene::Tape* tape_for_attributes,
                 const MeshingOptions& options, const std::vector<brick::BrickKey>* keys,
                 std::vector<BrickMeshRange>* out_ranges) {
    const int dim = cache.config().dim;
    const float vs = cache.config().voxel_size;
    auto global_sample = [&](int i, int j, int k) -> float {
        auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); };
        brick::BrickKey key{fdiv(i, dim), fdiv(j, dim), fdiv(k, dim)};
        return cache.sample(key, i - key.x * dim, j - key.y * dim, k - key.z * dim);
    };

    // The subset a consumer named, or every surface brick — which is the
    // export path and stays the default.
    std::vector<brick::BrickKey> owned;
    if (!keys) {
        owned = cache.surface_bricks();
        keys = &owned;
    }
    if (out_ranges) {
        out_ranges->clear();
        out_ranges->reserve(keys->size());
    }

    // ONE builder for every brick: lattice-edge welding spans brick seams,
    // keeping the result watertight across the sparse set.
    Builder b(cf3(0, 0, 0), vs);
    for (const brick::BrickKey& key : *keys) {
        const std::uint32_t v0 = static_cast<std::uint32_t>(b.out.positions.size());
        const std::uint32_t i0 = static_cast<std::uint32_t>(b.out.indices.size());
        int cmin[3] = {key.x * dim, key.y * dim, key.z * dim};
        int cmax[3] = {key.x * dim + dim, key.y * dim + dim, key.z * dim + dim};
        march_cells(b, global_sample, cmin, cmax);
        if (out_ranges)
            out_ranges->push_back({key, v0,
                                   static_cast<std::uint32_t>(b.out.positions.size()) - v0, i0,
                                   static_cast<std::uint32_t>(b.out.indices.size()) - i0});
    }
    Mesh m = std::move(b.out);
    if (tape_for_attributes) {
        apply_tape_attributes(m, *tape_for_attributes, options);
    } else if (options.normals == NormalMode::Face) {
        // Face normals are area-weighted from the triangles and need no field,
        // which is what NormalMode::Face means and what the C header promises a
        // caller who passes no document ("positions and face normals"). Without
        // this the promise was silently broken: attributes were applied only
        // through the tape, so a document-less brick mesh came back with no
        // normals at all and a host shaded it flat black.
        compute_face_normals(m);
    }
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
