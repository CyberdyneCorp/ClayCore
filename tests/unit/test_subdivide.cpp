// Catmull-Clark: the counts, the boundary, the poles, and DETERMINISM
// (mesh-multires spec, add-mesh-multires).
//
// Everything above this file treats a generated vertex's INDEX as its identity
// — detail is stored against it, undo records it, a document restores it — so
// the assertions that matter most here are not about geometry at all. They are
// that two runs produce the same bytes, and that a partial re-evaluation of a
// subset produces bit-for-bit what a full one would have produced for the same
// vertices. Get either wrong and every wrinkle in a saved file reattaches
// somewhere else.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/subdivide.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LevelConnectivity;
using mesh::LevelTopology;
using mesh::Mesh;

namespace {

// A quad grid on the XZ plane, `n` quads a side, spanning [-half, half]. Every
// coordinate is a multiple of a power of two, so the subdivision arithmetic is
// exact and an equality assertion is a fair one.
Mesh plane_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

// The unit cube as six quads, side 2, centred on the origin. Closed, all-quad,
// and every vertex valence 3 — the extraordinary case that a regular grid never
// exercises.
Mesh cube_quads() {
    Mesh m;
    m.positions = {cf3(-1, -1, -1), cf3(1, -1, -1), cf3(1, -1, 1), cf3(-1, -1, 1),
                   cf3(-1, 1, -1),  cf3(1, 1, -1),  cf3(1, 1, 1),  cf3(-1, 1, 1)};
    const std::uint32_t faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                       {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    for (const auto& f : faces) {
        m.quads.insert(m.quads.end(), {f[0], f[1], f[2], f[3]});
        m.indices.insert(m.indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
    }
    return m;
}

// A triangle cage: the regular tetrahedron. Catmull-Clark turns each triangle
// into three quads, so level 1 and everything above it is pure quads even here.
Mesh tetrahedron() {
    Mesh m;
    m.positions = {cf3(1, 1, 1), cf3(1, -1, -1), cf3(-1, 1, -1), cf3(-1, -1, 1)};
    m.indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    return m;
}

struct Level {
    LevelTopology topology;
    LevelConnectivity conn;
    std::vector<cfloat3> positions;
};

Level base_of(const Mesh& m) {
    Level l;
    const mesh::Adjacency adj = mesh::Adjacency::build(m, 0.0f);
    std::vector<std::uint32_t> class_of(m.positions.size());
    for (std::size_t v = 0; v < m.positions.size(); ++v)
        class_of[v] = adj.class_of(static_cast<std::uint32_t>(v));
    REQUIRE(mesh::base_topology_from_mesh(m, class_of.data(),
                                          static_cast<std::uint32_t>(adj.class_count()),
                                          &l.topology));
    l.positions.assign(l.topology.vertex_count, cf3(0, 0, 0));
    for (std::size_t v = 0; v < m.positions.size(); ++v) l.positions[class_of[v]] = m.positions[v];
    l.conn = LevelConnectivity::build(l.topology);
    return l;
}

Level subdivide_once(const Level& parent) {
    Level child;
    child.topology = mesh::subdivide_topology(parent.topology, parent.conn);
    mesh::subdivide_positions(parent.topology, parent.conn, parent.positions, &child.positions);
    child.conn = LevelConnectivity::build(child.topology);
    return child;
}

bool same_bytes(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

}  // namespace

TEST_CASE("one quad subdivides into the three-by-three grid, exactly") {
    // The whole face is border, so every rule here is the boundary rule: the
    // four corners are held (valence two), the four edge points are midpoints,
    // and the face point is the centroid. That makes the result the 3x3 lattice
    // of the quad and every coordinate exact.
    const Level base = base_of(plane_quads(1, 1.0f));
    CHECK(base.topology.face_count == 1);
    CHECK(base.conn.edges.size() == 4);

    const Level l1 = subdivide_once(base);
    CHECK(l1.topology.vertex_count == 9);   // 4 vertex + 4 edge + 1 face
    CHECK(l1.topology.face_count == 4);
    CHECK(l1.topology.uniform_quads());

    // The four corners of the original, untouched.
    for (std::uint32_t v = 0; v < 4; ++v) {
        CHECK(l1.positions[v].x == doctest::Approx(base.positions[v].x));
        CHECK(l1.positions[v].z == doctest::Approx(base.positions[v].z));
    }
    // The face point is the centre.
    const cfloat3 centre = l1.positions[8];
    CHECK(centre.x == doctest::Approx(0.0f));
    CHECK(centre.y == doctest::Approx(0.0f));
    CHECK(centre.z == doctest::Approx(0.0f));
    // Every edge point is a midpoint of two corners, so each lies on a border
    // line at distance exactly 1 from the centre.
    for (std::uint32_t v = 4; v < 8; ++v) {
        const float d = std::sqrt(l1.positions[v].x * l1.positions[v].x +
                                  l1.positions[v].z * l1.positions[v].z);
        CHECK(d == doctest::Approx(1.0f));
    }
}

TEST_CASE("an open border keeps its extent instead of shrinking away from it") {
    // The failure this pins is the one the boundary rule exists for: apply the
    // interior average along an open border and the sheet pulls in from its own
    // edge a little more at every level, which is invisible in a wireframe and
    // obvious the moment two such sheets are meant to meet.
    Level level = base_of(plane_quads(4, 2.0f));
    for (int i = 0; i < 3; ++i) {
        level = subdivide_once(level);
        float lo = 1e9f, hi = -1e9f, max_y = 0.0f;
        for (const cfloat3& p : level.positions) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
            max_y = std::max(max_y, std::fabs(p.y));
        }
        CHECK(lo == doctest::Approx(-2.0f));
        CHECK(hi == doctest::Approx(2.0f));
        // A flat sheet stays flat. Every rule here is an average of coplanar
        // points, so any drift off the plane is an arithmetic bug rather than a
        // rounding one.
        CHECK(max_y == doctest::Approx(0.0f));
    }
}

TEST_CASE("Euler's formula holds through four levels of a closed cage") {
    Level level = base_of(cube_quads());
    CHECK(level.topology.vertex_count == 8);
    CHECK(level.topology.face_count == 6);
    CHECK(level.conn.edges.size() == 12);
    CHECK_FALSE(level.conn.non_manifold);

    for (int i = 0; i < 4; ++i) {
        const std::uint32_t parent_v = level.topology.vertex_count;
        const std::uint32_t parent_f = level.topology.face_count;
        const std::uint32_t parent_e = static_cast<std::uint32_t>(level.conn.edges.size());
        const std::uint32_t parent_corners = static_cast<std::uint32_t>(level.topology.corners.size());
        level = subdivide_once(level);
        CHECK(level.topology.vertex_count == parent_v + parent_e + parent_f);
        CHECK(level.topology.face_count == parent_corners);
        CHECK(level.conn.edges.size() == 2u * parent_e + parent_corners);
        // A closed surface of genus zero: V - E + F == 2, at every level.
        const std::int64_t chi = static_cast<std::int64_t>(level.topology.vertex_count) -
                                 static_cast<std::int64_t>(level.conn.edges.size()) +
                                 static_cast<std::int64_t>(level.topology.face_count);
        CHECK(chi == 2);
        CHECK_FALSE(level.conn.non_manifold);
        // Not one open edge anywhere: a closed cage that develops a border has
        // lost a face, and every rule above would then take the wrong branch.
        for (const mesh::LevelEdge& e : level.conn.edges) CHECK_FALSE(e.boundary());
    }
}

TEST_CASE("the cube's symmetry survives, which is what a valence-three pole tests") {
    const Level base = base_of(cube_quads());
    const Level l1 = subdivide_once(base);

    // Face points first: the centroid of each cube face, exactly.
    const std::uint32_t face_base = base.topology.vertex_count +
                                    static_cast<std::uint32_t>(base.conn.edges.size());
    for (std::uint32_t f = 0; f < 6; ++f) {
        const cfloat3 p = l1.positions[face_base + f];
        const float r = std::fabs(p.x) + std::fabs(p.y) + std::fabs(p.z);
        CHECK(r == doctest::Approx(1.0f));  // a unit axis direction
    }
    // The eight corner vertex points are all the same distance from the origin,
    // and so are the twelve edge points. Any asymmetry is the vertex rule
    // reading its incidences in an order that depends on the container.
    float corner_r = 0.0f;
    for (std::uint32_t v = 0; v < 8; ++v) {
        const cfloat3 p = l1.positions[v];
        const float r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (v == 0) corner_r = r;
        CHECK(r == doctest::Approx(corner_r));
    }
    CHECK(corner_r < std::sqrt(3.0f));  // the cage shrank, as Catmull-Clark does
    float edge_r = 0.0f;
    for (std::uint32_t v = 8; v < face_base; ++v) {
        const cfloat3 p = l1.positions[v];
        const float r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (v == 8) edge_r = r;
        CHECK(r == doctest::Approx(edge_r));
    }
}

TEST_CASE("a triangle cage becomes quads at level one and stays finite") {
    const Level base = base_of(tetrahedron());
    CHECK(base.topology.face_count == 4);
    CHECK_FALSE(base.topology.uniform_quads());

    const Level l1 = subdivide_once(base);
    CHECK(l1.topology.uniform_quads());
    CHECK(l1.topology.face_count == 12);  // three quads per triangle
    CHECK(l1.topology.vertex_count == 4 + 6 + 4);
    for (const cfloat3& p : l1.positions) {
        CHECK(std::isfinite(p.x));
        CHECK(std::isfinite(p.y));
        CHECK(std::isfinite(p.z));
    }
}

TEST_CASE("poles of valence three, five and six subdivide finitely") {
    // A fan of `n` quads around a centre vertex, closed into a disc with an
    // open border. Character topology is full of these and a regular grid tests
    // none of them.
    for (int n : {3, 5, 6, 9}) {
        Mesh m;
        m.positions.push_back(cf3(0, 0, 0));
        for (int i = 0; i < 2 * n; ++i) {
            const float a = 6.28318530718f * static_cast<float>(i) / static_cast<float>(2 * n);
            const float r = (i % 2 == 0) ? 1.0f : 1.3f;
            m.positions.push_back(cf3(r * std::cos(a), 0.0f, r * std::sin(a)));
        }
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = 1 + static_cast<std::uint32_t>((2 * i) % (2 * n));
            const std::uint32_t b = 1 + static_cast<std::uint32_t>((2 * i + 1) % (2 * n));
            const std::uint32_t c = 1 + static_cast<std::uint32_t>((2 * i + 2) % (2 * n));
            m.quads.insert(m.quads.end(), {0u, a, b, c});
            m.indices.insert(m.indices.end(), {0u, a, b, 0u, b, c});
        }
        Level level = base_of(m);
        std::size_t count = 0;
        level.conn.faces_of(0, &count);
        CHECK(count == static_cast<std::size_t>(n));
        for (int i = 0; i < 3; ++i) {
            level = subdivide_once(level);
            for (const cfloat3& p : level.positions) {
                CHECK(std::isfinite(p.x));
                CHECK(std::isfinite(p.y));
                CHECK(std::isfinite(p.z));
            }
        }
    }
}

TEST_CASE("the hierarchy is byte-identical across runs") {
    // Determinism is the property everything above this file spends: a
    // generated vertex's index is its identity. Two independent constructions
    // of the same three levels must agree corner for corner and bit for bit.
    Level a = base_of(cube_quads());
    Level b = base_of(cube_quads());
    for (int i = 0; i < 3; ++i) {
        a = subdivide_once(a);
        b = subdivide_once(b);
        CHECK(a.topology.corners == b.topology.corners);
        CHECK(a.topology.face_patch == b.topology.face_patch);
        CHECK(a.topology.vertex_count == b.topology.vertex_count);
        CHECK(same_bytes(a.positions, b.positions));
    }
}

TEST_CASE("every face knows the base face it descends from") {
    Level level = base_of(cube_quads());
    const std::uint32_t patches = level.topology.face_count;
    std::vector<std::uint32_t> per_patch(patches, 0);
    for (int i = 0; i < 3; ++i) {
        level = subdivide_once(level);
        CHECK(level.topology.patch_count == patches);
        std::fill(per_patch.begin(), per_patch.end(), 0u);
        for (std::uint32_t f = 0; f < level.topology.face_count; ++f) {
            REQUIRE(level.topology.patch_of(f) < patches);
            per_patch[level.topology.patch_of(f)]++;
        }
        // A base face owns exactly 4^level of the level's faces, and the count
        // is the same for every patch because every base face is a quad.
        for (std::uint32_t p = 0; p < patches; ++p)
            CHECK(per_patch[p] == level.topology.face_count / patches);
    }
}

TEST_CASE("a partial re-evaluation is bit-identical to the full one") {
    // THE GATE dirty propagation rests on. `dirty_children` names the child
    // vertices a moved parent can reach; every other child must come out of a
    // partial pass exactly as it went in, and the ones it does write must match
    // the whole-level computation bit for bit — not approximately, because a
    // last-bit difference at level 2 is a visible seam by level 5.
    Level base = base_of(plane_quads(6, 3.0f));
    Level child = subdivide_once(base);
    const std::vector<cfloat3> before = child.positions;

    const std::uint32_t moved = 24;  // an interior vertex of the grid
    base.positions[moved] = base.positions[moved] + cf3(0.0f, 0.7f, 0.0f);

    std::vector<std::uint32_t> dirty;
    mesh::dirty_children(base.topology, base.conn, {moved}, &dirty);
    CHECK(!dirty.empty());
    CHECK(dirty.size() < child.positions.size());

    std::vector<cfloat3> partial = before;
    mesh::subdivide_positions_partial(base.topology, base.conn, base.positions, dirty, &partial);

    std::vector<cfloat3> full;
    mesh::subdivide_positions(base.topology, base.conn, base.positions, &full);

    std::vector<char> in_dirty(child.positions.size(), 0);
    for (std::uint32_t v : dirty) in_dirty[v] = 1;

    std::size_t changed_outside = 0;
    for (std::size_t v = 0; v < full.size(); ++v) {
        if (in_dirty[v]) {
            CHECK(partial[v].x == full[v].x);
            CHECK(partial[v].y == full[v].y);
            CHECK(partial[v].z == full[v].z);
        } else {
            // Nothing outside the dirty set moved AT ALL — which is what makes
            // the dirty set the honest account of the edit's reach rather than
            // a guess that happens to be close.
            CHECK(before[v].y == full[v].y);
            changed_outside += (partial[v].y != before[v].y) ? 1 : 0;
        }
    }
    CHECK(changed_outside == 0);
}
