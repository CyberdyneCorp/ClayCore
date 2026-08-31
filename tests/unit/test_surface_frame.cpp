// The frame a detail coefficient means something in (mesh-multires spec,
// add-mesh-multires).
//
// The artefact this file exists to prevent does not show up in a magnitude
// check. A frame that FLIPS between two evaluations rotates every coefficient
// stored against it, and what an artist sees is detail swimming across the
// surface while they push a form underneath it — visible in a render, invisible
// to any test that only asks whether the numbers are finite. So the assertions
// here are about sign, stability under deformation, and the one property the
// whole representation rests on: a wrinkle authored normal to a flat surface is
// still normal to it after the surface is bent.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/subdivide.h"
#include "clay/mesh/surface_frame.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LevelConnectivity;
using mesh::LevelTopology;
using mesh::Mesh;
using mesh::SurfaceFrame;

namespace {

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

void check_orthonormal(const SurfaceFrame& f) {
    CHECK(cdot(f.tangent, f.tangent) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(cdot(f.bitangent, f.bitangent) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(cdot(f.normal, f.normal) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(cdot(f.tangent, f.normal) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(cdot(f.tangent, f.bitangent) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(cdot(f.bitangent, f.normal) == doctest::Approx(0.0f).epsilon(1e-4));
    // Right-handed, which is what makes the coefficient triple mean the same
    // thing everywhere rather than mirroring on some vertices.
    const cfloat3 b = ccross(f.normal, f.tangent);
    CHECK(cdot(b, f.bitangent) == doctest::Approx(1.0f).epsilon(1e-4));
}

// One level up: the pure subdivision positions, their normals, and the frames
// transported onto them.
struct Transported {
    LevelTopology topology;
    LevelConnectivity conn;
    std::vector<cfloat3> positions, normals;
    std::vector<SurfaceFrame> frames;
};

Transported step(const Level& parent, const std::vector<SurfaceFrame>& parent_frames) {
    Transported t;
    t.topology = mesh::subdivide_topology(parent.topology, parent.conn);
    mesh::subdivide_positions(parent.topology, parent.conn, parent.positions, &t.positions);
    t.conn = LevelConnectivity::build(t.topology);
    mesh::level_normals(t.topology, t.conn, t.positions, &t.normals);
    mesh::transport_frames(parent.topology, parent.conn, parent_frames, t.normals, &t.frames);
    return t;
}

}  // namespace

TEST_CASE("a built frame is orthonormal and right-handed everywhere") {
    const Level base = base_of(plane_quads(4, 2.0f));
    std::vector<cfloat3> normals;
    mesh::level_normals(base.topology, base.conn, base.positions, &normals);
    std::vector<SurfaceFrame> frames;
    mesh::build_base_frames(base.topology, base.conn, base.positions, normals, nullptr, &frames);
    REQUIRE(frames.size() == base.topology.vertex_count);
    for (const SurfaceFrame& f : frames) check_orthonormal(f);
}

TEST_CASE("a transported frame is orthonormal at every level") {
    Level base = base_of(plane_quads(3, 1.5f));
    // Give the sheet some curvature, so the transport actually has to rotate
    // something rather than carrying an identity.
    for (cfloat3& p : base.positions) p.y = 0.25f * p.x * p.x - 0.15f * p.z * p.z;
    std::vector<cfloat3> normals;
    mesh::level_normals(base.topology, base.conn, base.positions, &normals);
    std::vector<SurfaceFrame> frames;
    mesh::build_base_frames(base.topology, base.conn, base.positions, normals, nullptr, &frames);

    Level level = base;
    std::vector<SurfaceFrame> current = frames;
    for (int i = 0; i < 3; ++i) {
        const Transported t = step(level, current);
        for (const SurfaceFrame& f : t.frames) check_orthonormal(f);
        level.topology = t.topology;
        level.conn = t.conn;
        level.positions = t.positions;
        current = t.frames;
    }
}

TEST_CASE("a small deformation does not flip a frame") {
    // The failure: a tangent re-derived from whichever neighbour is
    // geometrically first reverses when two candidates swap places, and every
    // coefficient stored against it rotates by a right angle. The tangent here
    // is chosen by INDEX, so nothing the geometry does can change which
    // neighbour it is.
    Level base = base_of(plane_quads(5, 2.0f));
    std::vector<cfloat3> n0;
    mesh::level_normals(base.topology, base.conn, base.positions, &n0);
    std::vector<SurfaceFrame> f0;
    mesh::build_base_frames(base.topology, base.conn, base.positions, n0, nullptr, &f0);

    for (std::uint32_t v = 0; v < base.topology.vertex_count; ++v) {
        const float s = static_cast<float>(v % 7) * 0.001f;
        base.positions[v].y += s;
        base.positions[v].x += 0.0007f * static_cast<float>((v % 3));
    }
    std::vector<cfloat3> n1;
    mesh::level_normals(base.topology, base.conn, base.positions, &n1);
    std::vector<SurfaceFrame> f1;
    mesh::build_base_frames(base.topology, base.conn, base.positions, n1, nullptr, &f1);

    for (std::uint32_t v = 0; v < base.topology.vertex_count; ++v) {
        CHECK(cdot(f0[v].tangent, f1[v].tangent) > 0.9f);
        CHECK(cdot(f0[v].bitangent, f1[v].bitangent) > 0.9f);
        CHECK(cdot(f0[v].normal, f1[v].normal) > 0.9f);
    }
}

TEST_CASE("a normal-only wrinkle stays normal to the surface after it is bent") {
    // THE PROPERTY THE WHOLE REPRESENTATION RESTS ON. A world-space delta would
    // pass this test on the flat sheet and fail it after the bend, which is
    // exactly the case the feature exists for.
    Level base = base_of(plane_quads(4, 2.0f));
    std::vector<cfloat3> bn;
    mesh::level_normals(base.topology, base.conn, base.positions, &bn);
    std::vector<SurfaceFrame> bf;
    mesh::build_base_frames(base.topology, base.conn, base.positions, bn, nullptr, &bf);

    const Transported flat = step(base, bf);
    const float height = 0.05f;

    // Now BEND the cage: rotate it about the x axis by 40 degrees, plus a
    // genuine curvature so the bend is not a rigid motion the frame could
    // survive by accident.
    Level bent = base;
    const float a = 0.6981317f;  // 40 degrees
    for (cfloat3& p : bent.positions) {
        p.y = 0.2f * p.z * p.z;
        const float y = p.y * std::cos(a) - p.z * std::sin(a);
        const float z = p.y * std::sin(a) + p.z * std::cos(a);
        p.y = y;
        p.z = z;
    }
    std::vector<cfloat3> bn2;
    mesh::level_normals(bent.topology, bent.conn, bent.positions, &bn2);
    std::vector<SurfaceFrame> bf2;
    mesh::build_base_frames(bent.topology, bent.conn, bent.positions, bn2, nullptr, &bf2);
    const Transported curved = step(bent, bf2);

    REQUIRE(flat.frames.size() == curved.frames.size());
    for (std::size_t v = 0; v < curved.frames.size(); ++v) {
        // The stored detail is unchanged — it is (0, 0, height) on both — and
        // what it reconstructs to is a displacement along the CURRENT normal.
        const cfloat3 offset = mesh::frame_to_world(curved.frames[v], 0.0f, 0.0f, height);
        CHECK(clength(offset) == doctest::Approx(height).epsilon(1e-4));
        CHECK(cdot(offset, curved.normals[v]) == doctest::Approx(height).epsilon(1e-3));
    }
    // And the flat case is the sanity floor: the same coefficients there point
    // straight up, which is what says the test is measuring the bend rather
    // than a frame that always agrees with itself.
    for (std::size_t v = 0; v < flat.frames.size(); ++v) {
        const cfloat3 offset = mesh::frame_to_world(flat.frames[v], 0.0f, 0.0f, height);
        CHECK(std::fabs(offset.y) == doctest::Approx(height).epsilon(1e-4));
    }
}

TEST_CASE("world and frame coordinates are inverses") {
    const Level base = base_of(plane_quads(3, 1.0f));
    std::vector<cfloat3> normals;
    mesh::level_normals(base.topology, base.conn, base.positions, &normals);
    std::vector<SurfaceFrame> frames;
    mesh::build_base_frames(base.topology, base.conn, base.positions, normals, nullptr, &frames);

    const cfloat3 d = cf3(0.13f, -0.42f, 0.77f);
    for (const SurfaceFrame& f : frames) {
        float t = 0, b = 0, n = 0;
        mesh::world_to_frame(f, d, &t, &b, &n);
        const cfloat3 back = mesh::frame_to_world(f, t, b, n);
        CHECK(back.x == doctest::Approx(d.x).epsilon(1e-5));
        CHECK(back.y == doctest::Approx(d.y).epsilon(1e-5));
        CHECK(back.z == doctest::Approx(d.z).epsilon(1e-5));
    }
}

TEST_CASE("the shortest arc takes one direction onto another and preserves length") {
    const cfloat3 from = cnormalize(cf3(0.2f, 1.0f, -0.3f));
    const cfloat3 to = cnormalize(cf3(-0.7f, 0.4f, 0.5f));
    const cfloat3 moved = mesh::rotate_shortest_arc(from, from, to);
    CHECK(moved.x == doctest::Approx(to.x).epsilon(1e-5));
    CHECK(moved.y == doctest::Approx(to.y).epsilon(1e-5));
    CHECK(moved.z == doctest::Approx(to.z).epsilon(1e-5));

    const cfloat3 v = cf3(1.0f, 0.0f, 0.0f);
    CHECK(clength(mesh::rotate_shortest_arc(v, from, to)) == doctest::Approx(1.0f).epsilon(1e-5));

    // Aligned and antipodal, the two cases the general formula divides by zero
    // on. Neither may produce a NaN.
    const cfloat3 same = mesh::rotate_shortest_arc(v, from, from);
    CHECK(same.x == doctest::Approx(v.x));
    const cfloat3 flipped = mesh::rotate_shortest_arc(v, from, -from);
    CHECK(std::isfinite(flipped.x));
    CHECK(clength(flipped) == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("frames are deterministic across two independent builds") {
    Level a = base_of(plane_quads(4, 2.0f));
    for (cfloat3& p : a.positions) p.y = 0.3f * std::sin(p.x) * std::cos(p.z);
    Level b = a;

    std::vector<cfloat3> na, nb;
    mesh::level_normals(a.topology, a.conn, a.positions, &na);
    mesh::level_normals(b.topology, b.conn, b.positions, &nb);
    std::vector<SurfaceFrame> fa, fb;
    mesh::build_base_frames(a.topology, a.conn, a.positions, na, nullptr, &fa);
    mesh::build_base_frames(b.topology, b.conn, b.positions, nb, nullptr, &fb);

    Transported ta = step(a, fa), tb = step(b, fb);
    REQUIRE(ta.frames.size() == tb.frames.size());
    for (std::size_t v = 0; v < ta.frames.size(); ++v) {
        CHECK(ta.frames[v].tangent.x == tb.frames[v].tangent.x);
        CHECK(ta.frames[v].tangent.y == tb.frames[v].tangent.y);
        CHECK(ta.frames[v].tangent.z == tb.frames[v].tangent.z);
        CHECK(ta.frames[v].normal.x == tb.frames[v].normal.x);
    }
}

TEST_CASE("a partial frame and normal update matches the full one bit for bit") {
    Level base = base_of(plane_quads(6, 3.0f));
    for (cfloat3& p : base.positions) p.y = 0.2f * p.x * p.z;
    std::vector<cfloat3> bn;
    mesh::level_normals(base.topology, base.conn, base.positions, &bn);
    std::vector<SurfaceFrame> bf;
    mesh::build_base_frames(base.topology, base.conn, base.positions, bn, nullptr, &bf);

    const Transported full_before = step(base, bf);

    const std::uint32_t moved = 24;
    base.positions[moved].y += 0.5f;
    std::vector<std::uint32_t> dirty;
    mesh::dirty_children(base.topology, base.conn, {moved}, &dirty);

    // The partial path: subdivide, renormal and retransport only the dirty set.
    std::vector<cfloat3> positions = full_before.positions;
    mesh::subdivide_positions_partial(base.topology, base.conn, base.positions, dirty, &positions);
    std::vector<cfloat3> normals = full_before.normals;
    mesh::level_normals_partial(full_before.topology, full_before.conn, positions, dirty, &normals);
    std::vector<SurfaceFrame> frames = full_before.frames;
    mesh::transport_frames_partial(base.topology, base.conn, bf, normals, dirty, &frames);

    const Transported full_after = step(base, bf);
    for (std::uint32_t v : dirty) {
        CHECK(positions[v].y == full_after.positions[v].y);
        CHECK(frames[v].tangent.x == full_after.frames[v].tangent.x);
        CHECK(frames[v].normal.y == full_after.frames[v].normal.y);
    }
}
