// A lattice cage over a mesh layer (meshing spec, lattice-on-a-mesh).
//
// This is the ZBrush/Blender feature, and it is available here for a specific
// reason worth restating in the tests: it runs FORWARD. A mesh knows where its
// vertices are, so nothing is inverted, nothing iterates, and nothing is
// approximated — which is why the assertions below are equalities rather than
// tolerances wherever the arithmetic allows.
//
// The contract every mesh verb holds applies unchanged: topology never
// changes, and it is compared byte for byte rather than by count.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay/mesh/lattice.h"
#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::Lattice;
using mesh::Mesh;
using mesh::MeshSculptor;
using mesh::VertexDeltas;

namespace {

// A box of quads-as-triangles spanning [-half, half], subdivided n per side,
// so there are interior vertices for a cage to bend rather than only corners.
Mesh grid_plane(int n, float half) {
    Mesh m;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const float x = -half + 2.0f * half * static_cast<float>(i) / static_cast<float>(n);
            const float z = -half + 2.0f * half * static_cast<float>(j) / static_cast<float>(n);
            m.positions.push_back(cf3(x, 0.0f, z));
            m.normals.push_back(cf3(0, 1, 0));
        }
    const auto at = [n](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            m.indices.push_back(at(i, j));
            m.indices.push_back(at(i, j + 1));
            m.indices.push_back(at(i + 1, j + 1));
            m.indices.push_back(at(i, j));
            m.indices.push_back(at(i + 1, j + 1));
            m.indices.push_back(at(i + 1, j));
        }
    return m;
}

math::Aabb bounds_of(const Mesh& m) {
    math::Aabb b;
    for (const cfloat3& p : m.positions) b.expand(p);
    return b;
}

// Every control point dragged by the same vector.
Lattice uniform(const math::Aabb& box, cfloat3 by, int n = 3) {
    Lattice cage(box, n, n, n);
    for (int k = 0; k < cage.nz(); ++k)
        for (int j = 0; j < cage.ny(); ++j)
            for (int i = 0; i < cage.nx(); ++i) cage.set_offset(i, j, k, by);
    return cage;
}

}  // namespace

TEST_CASE("an untouched cage changes nothing at all") {
    // Offsets rather than positions is what buys this: the identity is exact
    // by construction, not to within a tolerance.
    Mesh m = grid_plane(6, 1.0f);
    const Mesh before = m;
    Lattice cage(bounds_of(m), 4, 3, 5);
    CHECK(cage.is_identity());

    MeshSculptor sculptor(m);
    CHECK(sculptor.apply_lattice(cage) == 0);
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        CAPTURE(v);
        CHECK(m.positions[v].x == before.positions[v].x);
        CHECK(m.positions[v].y == before.positions[v].y);
        CHECK(m.positions[v].z == before.positions[v].z);
        CHECK(m.normals[v].y == before.normals[v].y);
    }
    // And the displacement field itself is zero, not merely small.
    CHECK(cage.displacement(cf3(0.3f, -0.2f, 0.7f)).x == 0.0f);
    CHECK(cage.displacement(cf3(0.3f, -0.2f, 0.7f)).y == 0.0f);
    CHECK(cage.displacement(cf3(0.3f, -0.2f, 0.7f)).z == 0.0f);
}

TEST_CASE("a uniformly dragged cage translates the mesh exactly") {
    // The Bernstein basis is a partition of unity, so every point picks up the
    // same offset — which makes this an equality rather than an approximation.
    Mesh m = grid_plane(5, 1.0f);
    const Mesh before = m;
    const cfloat3 by = cf3(0.25f, -0.5f, 0.125f);

    MeshSculptor sculptor(m);
    CHECK(sculptor.apply_lattice(uniform(bounds_of(m), by)) == m.positions.size());
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        CAPTURE(v);
        CHECK(m.positions[v].x == doctest::Approx(before.positions[v].x + by.x).epsilon(1e-6));
        CHECK(m.positions[v].y == doctest::Approx(before.positions[v].y + by.y).epsilon(1e-6));
        CHECK(m.positions[v].z == doctest::Approx(before.positions[v].z + by.z).epsilon(1e-6));
    }
}

TEST_CASE("a two-per-axis cage is exactly trilinear") {
    // Degree is one less than the point count, so 2x2x2 is degree one on every
    // axis. Checked against an independently written trilinear blend rather
    // than against the implementation's own arithmetic.
    const math::Aabb box{cf3(-1, -1, -1), cf3(1, 1, 1)};
    Lattice cage(box, 2, 2, 2);
    cfloat3 corner[8];
    for (int c = 0; c < 8; ++c) {
        corner[c] = cf3(0.1f * static_cast<float>(c + 1), -0.05f * static_cast<float>(c),
                        0.02f * static_cast<float>(c * c));
        cage.set_offset(c & 1, (c >> 1) & 1, (c >> 2) & 1, corner[c]);
    }

    for (float z = -1.0f; z <= 1.0f; z += 0.4f)
        for (float y = -1.0f; y <= 1.0f; y += 0.4f)
            for (float x = -1.0f; x <= 1.0f; x += 0.4f) {
                const float s = (x + 1.0f) * 0.5f, t = (y + 1.0f) * 0.5f, u = (z + 1.0f) * 0.5f;
                cfloat3 want = cf3(0, 0, 0);
                for (int c = 0; c < 8; ++c) {
                    const float wx = (c & 1) ? s : 1.0f - s;
                    const float wy = ((c >> 1) & 1) ? t : 1.0f - t;
                    const float wz = ((c >> 2) & 1) ? u : 1.0f - u;
                    want = want + corner[c] * (wx * wy * wz);
                }
                const cfloat3 got = cage.displacement(cf3(x, y, z));
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(z);
                CHECK(got.x == doctest::Approx(want.x).epsilon(1e-5));
                CHECK(got.y == doctest::Approx(want.y).epsilon(1e-5));
                CHECK(got.z == doctest::Approx(want.z).epsilon(1e-5));
            }
}

TEST_CASE("a corner control point moves that corner exactly") {
    // Bernstein INTERPOLATES its end points, which is what makes a lattice UI
    // behave: dragging a corner handle takes that corner of the box with it,
    // rather than a fraction of the way.
    const math::Aabb box{cf3(0, 0, 0), cf3(1, 1, 1)};
    Lattice cage(box, 3, 3, 3);
    const cfloat3 pull = cf3(0.5f, 0.25f, -0.125f);
    cage.set_offset(0, 0, 0, pull);

    const cfloat3 at_corner = cage.displacement(cf3(0, 0, 0));
    CHECK(at_corner.x == doctest::Approx(pull.x).epsilon(1e-6));
    CHECK(at_corner.y == doctest::Approx(pull.y).epsilon(1e-6));
    CHECK(at_corner.z == doctest::Approx(pull.z).epsilon(1e-6));
    // The far corner is untouched by it.
    const cfloat3 far = cage.displacement(cf3(1, 1, 1));
    CHECK(far.x == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(far.y == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("material outside the box travels rigidly, it is not drawn onto it") {
    // The mistake this formulation exists to avoid: evaluating a POSITION cage
    // at a clamped parameter would collapse every outside vertex onto the
    // box's surface. Offsets plus a clamp carry it along instead.
    const math::Aabb box{cf3(-1, -1, -1), cf3(1, 1, 1)};
    const cfloat3 by = cf3(0.0f, 0.5f, 0.0f);
    Lattice cage = uniform(box, by);

    // A point well outside on +X takes the same offset as the face it is past,
    // and keeps its own X.
    const cfloat3 outside = cf3(5.0f, 0.0f, 0.0f);
    const cfloat3 d = cage.displacement(outside);
    CHECK(d.y == doctest::Approx(by.y).epsilon(1e-6));
    const cfloat3 moved = outside + d;
    CHECK(moved.x == doctest::Approx(5.0f).epsilon(1e-6));  // NOT pulled back to the box

    // With only one corner dragged, an outside point takes the nearest corner's
    // share rather than the whole drag or none of it.
    Lattice one(box, 2, 2, 2);
    one.set_offset(1, 1, 1, cf3(1, 0, 0));
    const cfloat3 past_that_corner = one.displacement(cf3(9.0f, 9.0f, 9.0f));
    CHECK(past_that_corner.x == doctest::Approx(1.0f).epsilon(1e-6));
    const cfloat3 past_the_other = one.displacement(cf3(-9.0f, -9.0f, -9.0f));
    CHECK(past_the_other.x == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("a lattice does not touch topology") {
    Mesh m = grid_plane(4, 1.0f);
    // A quad buffer too, since a lattice must leave one exactly as it found it.
    m.quads = {0, 1, 2, 3};
    const std::vector<std::uint32_t> indices = m.indices;
    const std::vector<std::uint32_t> quads = m.quads;

    Lattice cage(bounds_of(m), 3, 3, 3);
    cage.set_offset(1, 1, 1, cf3(0.4f, 0.9f, -0.2f));
    cage.set_offset(0, 2, 2, cf3(-0.3f, 0.1f, 0.5f));

    MeshSculptor sculptor(m);
    CHECK(sculptor.apply_lattice(cage) > 0);
    CHECK(m.indices == indices);
    CHECK(m.quads == quads);
    CHECK(m.positions.size() == m.normals.size());
}

TEST_CASE("a lattice is one undo step") {
    Mesh m = grid_plane(5, 1.0f);
    const Mesh before = m;

    Lattice cage(bounds_of(m), 3, 3, 3);
    cage.set_offset(1, 1, 1, cf3(0.2f, 0.8f, 0.1f));
    cage.set_offset(2, 0, 1, cf3(-0.4f, 0.3f, 0.0f));

    VertexDeltas record;
    MeshSculptor sculptor(m);
    CHECK(sculptor.apply_lattice(cage, &record) > 0);
    CHECK(!record.empty());
    REQUIRE(record.revert(m));

    // Bit-identical, positions AND normals — the bar the brushes hold.
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        CAPTURE(v);
        CHECK(std::memcmp(&m.positions[v], &before.positions[v], sizeof(cfloat3)) == 0);
        CHECK(std::memcmp(&m.normals[v], &before.normals[v], sizeof(cfloat3)) == 0);
    }
    // ...and re-applying the record puts it back. Checked over the whole mesh
    // rather than at vertex 0, which is a CORNER — Bernstein interpolates its
    // end points, so an interior control point correctly leaves the corners
    // exactly where they were.
    REQUIRE(record.apply(m));
    int differs = 0;
    for (std::size_t v = 0; v < m.positions.size(); ++v)
        if (std::memcmp(&m.positions[v], &before.positions[v], sizeof(cfloat3)) != 0) ++differs;
    CHECK(differs > 0);
}

TEST_CASE("normals follow the vertices a cage moved") {
    Mesh m = grid_plane(6, 1.0f);
    Lattice cage(bounds_of(m), 3, 3, 3);
    cage.set_offset(1, 1, 1, cf3(0.0f, 1.2f, 0.0f));  // pull the middle up

    MeshSculptor sculptor(m);
    REQUIRE(sculptor.apply_lattice(cage) > 0);
    // The plane was flat with every normal +Y; bending it must tilt some.
    int tilted = 0;
    for (const cfloat3& n : m.normals)
        if (n.y < 0.999f) ++tilted;
    CHECK(tilted > 0);
    // ...and every normal is still a unit vector.
    for (std::size_t v = 0; v < m.normals.size(); ++v) {
        CAPTURE(v);
        CHECK(clength(m.normals[v]) == doctest::Approx(1.0f).epsilon(1e-4));
    }
}

TEST_CASE("a cage refuses what it cannot mean, without failing") {
    // A resolution below two cannot span an axis, and one above the cap is a
    // slider typo rather than a request. Both are clamped rather than refused:
    // a cage is an editable object, and there is a sane cage on both sides.
    const math::Aabb box{cf3(0, 0, 0), cf3(1, 1, 1)};
    Lattice low(box, 1, 0, -3);
    CHECK(low.nx() == mesh::kMinLatticeDivisions);
    CHECK(low.ny() == mesh::kMinLatticeDivisions);
    CHECK(low.nz() == mesh::kMinLatticeDivisions);
    Lattice high(box, 9999, 9999, 9999);
    CHECK(high.nx() == mesh::kMaxLatticeDivisions);

    // An empty box has nothing to span, so the cage is the identity and says so
    // rather than dividing by a zero extent.
    Lattice nothing(math::Aabb{}, 3, 3, 3);
    CHECK(nothing.is_identity());
    nothing.set_offset(1, 1, 1, cf3(1, 1, 1));
    const cfloat3 d = nothing.displacement(cf3(0.5f, 0.5f, 0.5f));
    CHECK(d.x == 0.0f);
    CHECK(d.y == 0.0f);
    CHECK(d.z == 0.0f);

    // An out-of-range control point reads zero and writes nowhere, rather than
    // running off the array.
    Lattice cage(box, 3, 3, 3);
    cage.set_offset(9, 9, 9, cf3(5, 5, 5));
    CHECK(cage.is_identity());
    CHECK(cage.offset(-1, 0, 0).x == 0.0f);
}

TEST_CASE("a flat cage still works on the axes that are not flat") {
    // A cage over a plane's own bounds is zero-thickness on Y. That axis has
    // no parameter to read, and the other two must still deform.
    Mesh m = grid_plane(4, 1.0f);
    const math::Aabb flat = bounds_of(m);
    CHECK(flat.max.y == flat.min.y);

    Lattice cage(flat, 3, 3, 3);
    cage.set_offset(2, 0, 2, cf3(0.0f, 0.7f, 0.0f));
    MeshSculptor sculptor(m);
    CHECK(sculptor.apply_lattice(cage) > 0);

    // The corner nearest that control point rose the most.
    float highest = -1e9f;
    for (const cfloat3& p : m.positions) highest = std::max(highest, p.y);
    CHECK(highest > 0.1f);
}
