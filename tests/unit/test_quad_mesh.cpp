// Quad meshing: the quads the dual mesher already built, kept.
//
// Two things are asserted everywhere here. The first is the INVARIANT — the
// triangles are exactly the quads' expansion — because a quad list that
// disagrees with the triangles beside it is a mesh whose two index arrays
// describe different surfaces, and it can be saved into a document before
// anyone notices. The second is that nothing that existed before this change
// moved: the triangle meshers, their meshes and their exported bytes.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/eval/backend.h"
#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/surface_nets.h"
#include "clay/scene/bounds.h"
#include "clay/voxel/grid.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using mesh::Mesh;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

scene::Tape sphere_tape(float r) {
    static scene::Document doc;
    doc = scene::Document{};
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::sphere(r), cf3(0, 0, 0));
    n.color = cf3(0.8f, 0.2f, 0.1f);
    l.sdf->insert(n);
    return scene::compile_document(doc);
}

const math::Aabb kSphereRegion{cf3(-1.5f, -1.5f, -1.5f), cf3(1.5f, 1.5f, 1.5f)};

// Bit-identical, not approximately equal: the point of keeping the quad
// meshers on one code path with the triangle meshers is that the surface
// cannot drift, and Approx() would hide exactly the drift worth catching.
bool same_geometry(const Mesh& a, const Mesh& b) {
    if (a.positions.size() != b.positions.size()) return false;
    if (a.indices != b.indices) return false;
    for (std::size_t i = 0; i < a.positions.size(); ++i) {
        if (a.positions[i].x != b.positions[i].x) return false;
        if (a.positions[i].y != b.positions[i].y) return false;
        if (a.positions[i].z != b.positions[i].z) return false;
    }
    return true;
}

VoxelGrid cube_grid(int n, float voxel = 0.1f) {
    VoxelGrid g(voxel);
    const std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.fill_box({0, 0, 0}, {n - 1, n - 1, n - 1}, c);
    return g;
}

}  // namespace

TEST_CASE("a quad mesh's triangles are its quads") {
    scene::Tape t = sphere_tape(1.0f);
    const Mesh q = mesh::mesh_tape_quads(t, kSphereRegion, 0.1f);

    REQUIRE(q.quad_count() > 0);
    CHECK(q.indices.size() == q.quad_count() * 6);
    CHECK(mesh::quads_consistent(q));
    // Spelled out once rather than trusting the checker that the rest of the
    // file leans on.
    for (std::size_t i = 0; i < q.quad_count(); ++i) {
        const std::uint32_t* c = &q.quads[i * 4];
        CHECK(q.indices[i * 6 + 0] == c[0]);
        CHECK(q.indices[i * 6 + 1] == c[1]);
        CHECK(q.indices[i * 6 + 2] == c[2]);
        CHECK(q.indices[i * 6 + 3] == c[0]);
        CHECK(q.indices[i * 6 + 4] == c[2]);
        CHECK(q.indices[i * 6 + 5] == c[3]);
    }
}

TEST_CASE("the quad mesh IS the nets mesh") {
    scene::Tape t = sphere_tape(1.0f);
    const Mesh nets = mesh::mesh_tape_nets(t, kSphereRegion, 0.1f);
    const Mesh quads = mesh::mesh_tape_quads(t, kSphereRegion, 0.1f);

    CHECK(same_geometry(nets, quads));
    CHECK(nets.quads.empty());  // no mesher starts carrying quads on its own
    CHECK(quads.quad_count() == nets.triangle_count() / 2);
}

// quad_mesh.h states that a thin symmetric feature produces a few
// near-degenerate faces and that they are INHERITED rather than introduced.
// The second half is the load-bearing half — it is what says this feature adds
// no defect — so it is measured on a shape that actually produces them, not
// only on the sphere above where none appear.
TEST_CASE("the near-degenerate faces of a thin shape are the nets mesh's own") {
    static scene::Document doc;
    doc = scene::Document{};
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::capsule(cf3(-0.3f, 0, 0), cf3(0.3f, 0, 0), 0.02f),
                       cf3(0, 0, 0)));
    scene::Tape t = scene::compile_document(doc);
    const math::Aabb region{cf3(-0.5f, -0.2f, -0.2f), cf3(0.5f, 0.2f, 0.2f)};

    const Mesh nets = mesh::mesh_tape_nets(t, region, 0.02f);
    const Mesh quads = mesh::mesh_tape_quads(t, region, 0.02f);
    REQUIRE(quads.quad_count() > 0);
    CHECK(same_geometry(nets, quads));

    auto tiny_triangles = [](const Mesh& m) {
        std::size_t n = 0;
        for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const cfloat3 a = m.positions[m.indices[i]];
            const cfloat3 b = m.positions[m.indices[i + 1]];
            const cfloat3 c = m.positions[m.indices[i + 2]];
            if (clength(ccross(b - a, c - a)) < 1e-12f) ++n;
        }
        return n;
    };
    // The two thin walls of the capsule average to the same point in a cell
    // the axis passes through, which collapses a face. Whatever the count is,
    // the triangle path shipped exactly the same ones before quads existed.
    CHECK(tiny_triangles(quads) > 0);
    CHECK(tiny_triangles(quads) == tiny_triangles(nets));
}

TEST_CASE("every existing mesher returns an empty quad array") {
    scene::Tape t = sphere_tape(1.0f);
    CHECK(mesh::mesh_tape(t, kSphereRegion, 0.15f).quads.empty());
    CHECK(mesh::mesh_tape_nets(t, kSphereRegion, 0.15f).quads.empty());

    VoxelGrid g = cube_grid(6);
    CHECK(g.mesh_greedy().quads.empty());
    CHECK(g.mesh_smooth().quads.empty());
    CHECK(g.mesh_greedy_chunks(g.occupied_chunk_keys()).quads.empty());

    // The brick mesher too: it is the one dense mesher that does not take a
    // region, so an "every mesher" claim that skipped it would be a claim
    // about the ones that were convenient to reach.
    scene::Document doc = clay_test::gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    brick::BrickCache cache(brick::BrickConfig{8, 0.08f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape culled = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(culled, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }
    const Mesh bricks = mesh::mesh_bricks(cache, &doc);
    REQUIRE(!bricks.empty());
    CHECK(bricks.quads.empty());
}

TEST_CASE("the dual's quads average four to a vertex and never meet mid-edge") {
    // The reason greedy MERGING was rejected as the quad source. A T-junction
    // — a vertex sitting in the interior of another face's edge — cracks under
    // subdivision and splits normals, so "clean quads" means these two
    // properties, not merely "four corners".
    scene::Tape t = sphere_tape(1.0f);
    const Mesh q = mesh::mesh_tape_quads(t, kSphereRegion, 0.1f);
    REQUIRE(q.quad_count() > 100);

    std::map<std::uint32_t, int> valence;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_uses;
    for (std::size_t i = 0; i < q.quad_count(); ++i) {
        const std::uint32_t* c = &q.quads[i * 4];
        for (int k = 0; k < 4; ++k) {
            valence[c[k]] += 1;
            const std::uint32_t a = c[k], b = c[(k + 1) % 4];
            edge_uses[{std::min(a, b), std::max(a, b)}] += 1;
        }
    }

    // Valence AVERAGES four and is not four everywhere: a cell the surface
    // enters through a corner rather than through a face has six of its twelve
    // lattice edges change sign and so belongs to six quads, and one clipped
    // by a corner belongs to three. That is the lattice's discrete curvature
    // showing, it does not diminish with resolution, and it is the honest
    // shape of this mesher — a claim of valence four everywhere would be a
    // claim of retopology. What matters for subdivision is the range being
    // small and closed, so no vertex is a fan.
    std::size_t four = 0;
    for (const auto& [vertex, n] : valence) {
        CHECK(n >= 3);
        CHECK(n <= 6);
        if (n == 4) ++four;
    }
    CHECK(four * 2 > valence.size());  // four is still the plurality
    // Sum of valences is 4 per quad by construction, so the mean is 4V'/V ~ 4.
    std::size_t corners = 0;
    for (const auto& [vertex, n] : valence) corners += static_cast<std::size_t>(n);
    CHECK(corners == q.quads.size());

    // No T-junction: every quad edge is shared by at most two quads, and a
    // vertex never lies in the interior of an edge it does not end. The lattice
    // dual gives the second for free — an edge's endpoints are the two cells
    // around a lattice face, and no third cell vertex can land between them —
    // so the check that has teeth is the first.
    for (const auto& [edge, n] : edge_uses) CHECK(n <= 2);
}

TEST_CASE("a quad mesh of a sphere is quads all the way") {
    scene::Tape t = sphere_tape(1.0f);
    const Mesh q = mesh::mesh_tape_quads(t, kSphereRegion, 0.12f);
    // Every face is a quad: there is no triangle in the output that is not
    // half of one, which is what indices.size() == 6 * quad_count says.
    REQUIRE(q.quad_count() > 0);
    CHECK(q.indices.size() == q.quad_count() * 6);
    CHECK(q.normals.size() == q.positions.size());
    CHECK(q.colors.size() == q.positions.size());
}

TEST_CASE("an unaffordable or malformed cell size is refused, not allocated") {
    scene::Tape t = sphere_tape(1.0f);
    for (float bad : {0.0f, -0.1f, 1e-7f}) {
        const Mesh m = mesh::mesh_tape_quads(t, kSphereRegion, bad);
        CHECK(m.empty());
        CHECK(m.quads.empty());
    }
}

TEST_CASE("voxel dual mode at the grid's own voxel size IS the smooth mesh") {
    // The gate that keeps the two on one code path. If the generalised cell
    // size ever stops reproducing the smooth mesher's lattice origin and
    // range exactly, this fails rather than the difference reaching a host as
    // "the sculpt moved".
    VoxelGrid g = cube_grid(8);
    const Mesh smooth = g.mesh_smooth();
    const Mesh quads = g.mesh_quads();

    REQUIRE(smooth.positions.size() > 0);
    CHECK(same_geometry(smooth, quads));
    CHECK(quads.quad_count() == smooth.triangle_count() / 2);
    CHECK(mesh::quads_consistent(quads));
    CHECK(smooth.quads.empty());
}

TEST_CASE("the identity holds per LEVEL, which is not the level a default call picks") {
    // mesh_smooth() with no level follows the ACTIVE level; QuadOptions::level
    // defaults to 0 because in faces mode it is the count lever. So the gate
    // above is an identity between the same level's two meshers, and on a
    // multi-level grid the two DEFAULT calls are meshing different things —
    // stated here so the difference is a decision rather than a surprise.
    VoxelGrid g = cube_grid(8);
    REQUIRE(g.add_level() == 1);
    REQUIRE(g.set_active_level(1));
    const std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.fill_box({0, 0, 0}, {5, 5, 5}, c);

    VoxelGrid::QuadOptions fine_level;
    fine_level.level = 1;
    CHECK(same_geometry(g.mesh_smooth(1), g.mesh_quads(fine_level)));
    CHECK(same_geometry(g.mesh_smooth(0), g.mesh_quads()));
    CHECK_FALSE(same_geometry(g.mesh_smooth(), g.mesh_quads()));  // active is 1, options is 0
}

TEST_CASE("a cell finer than a voxel is clamped to the voxel size") {
    VoxelGrid g = cube_grid(6);
    VoxelGrid::QuadOptions fine;
    fine.cell_size = g.voxel_size() * 0.25f;
    CHECK(same_geometry(g.mesh_quads(), g.mesh_quads(fine)));
}

TEST_CASE("a coarser cell buys fewer quads") {
    VoxelGrid g = cube_grid(16);
    VoxelGrid::QuadOptions coarse;
    coarse.cell_size = g.voxel_size() * 3.0f;
    const Mesh fine = g.mesh_quads();
    const Mesh c = g.mesh_quads(coarse);
    REQUIRE(c.quad_count() > 0);
    CHECK(c.quad_count() < fine.quad_count());
    CHECK(mesh::quads_consistent(c));

    // And it still covers the whole sculpt. The coarse path computes its own
    // lattice range from the occupancy box rather than inheriting the box's
    // cells, so an off-by-one there would silently CLIP the model instead of
    // failing — a shrunken bounding box is what that looks like.
    auto bounds = [](const Mesh& m) {
        math::Aabb b;
        for (const cfloat3& p : m.positions) b.expand(p);
        return b;
    };
    const math::Aabb fb = bounds(fine), cb = bounds(c);
    const float slack = coarse.cell_size * 1.5f;
    CHECK(cb.min.x <= fb.min.x + slack);
    CHECK(cb.min.y <= fb.min.y + slack);
    CHECK(cb.min.z <= fb.min.z + slack);
    CHECK(cb.max.x >= fb.max.x - slack);
    CHECK(cb.max.y >= fb.max.y - slack);
    CHECK(cb.max.z >= fb.max.z - slack);
}

TEST_CASE("faces mode is one quad per exposed voxel face") {
    VoxelGrid g = cube_grid(4);
    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;
    const Mesh m = g.mesh_quads(faces);

    // A 4^3 cube exposes 6 * 16 faces.
    CHECK(m.quad_count() == 6 * 16);
    CHECK(mesh::quads_consistent(m));
    // Welded: a 4x4 face grid has 5x5 corners, and the cube's 6 faces share
    // their border corners, so the count is the cube's surface lattice —
    // 5^3 minus the 3^3 interior points.
    CHECK(m.positions.size() == 5 * 5 * 5 - 3 * 3 * 3);
    // No vertex normals: a welded corner faces three ways at once.
    CHECK(m.normals.empty());
    CHECK(m.colors.size() == m.positions.size());

    // The same surface greedy meshing covers, at the same total area.
    const Mesh greedy = g.mesh_greedy();
    auto area = [](const Mesh& mesh) {
        double total = 0.0;
        for (std::size_t t = 0; t < mesh.triangle_count(); ++t) {
            const cfloat3 a = mesh.positions[mesh.indices[t * 3]];
            const cfloat3 b = mesh.positions[mesh.indices[t * 3 + 1]];
            const cfloat3 c = mesh.positions[mesh.indices[t * 3 + 2]];
            total += 0.5 * static_cast<double>(clength(ccross(b - a, c - a)));
        }
        return total;
    };
    CHECK(area(m) == doctest::Approx(area(greedy)).epsilon(1e-4));
}

TEST_CASE("faces mode welds within a colour and splits across one") {
    VoxelGrid g(0.1f);
    const std::uint8_t red = g.palette_add(cf3(1, 0, 0));
    const std::uint8_t blue = g.palette_add(cf3(0, 0, 1));
    g.fill_box({0, 0, 0}, {1, 0, 0}, red);
    g.fill_box({2, 0, 0}, {3, 0, 0}, blue);

    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;
    const Mesh m = g.mesh_quads(faces);
    REQUIRE(m.quad_count() > 0);

    // Count the vertices at the lattice corners on the colour boundary plane
    // (x = 2 voxels). Each such corner is shared by red faces and blue faces
    // and must appear once per colour: welding within a colour, splitting
    // across one, which is what keeps per-face palette colour.
    int on_boundary = 0;
    std::set<std::pair<int, int>> boundary_corners;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const cfloat3 p = m.positions[i];
        if (p.x != doctest::Approx(0.2f)) continue;
        ++on_boundary;
        boundary_corners.insert({static_cast<int>(p.y * 100.0f + 0.5f),
                                 static_cast<int>(p.z * 100.0f + 0.5f)});
    }
    CHECK(boundary_corners.size() == 4);      // the 2x2 corners of that plane
    CHECK(on_boundary == 8);                  // each one twice: red and blue
}

TEST_CASE("faces mode winds every face outward, on both signs of all three axes") {
    // emit_quad flips its triangle order per axis and sign; the quad corner
    // order has to flip with it or the quads and their triangulation disagree
    // about which way the face points — silent until an import with backface
    // culling on.
    VoxelGrid g(1.0f);
    const std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({0, 0, 0}, c);

    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;
    const Mesh m = g.mesh_quads(faces);
    REQUIRE(m.quad_count() == 6);

    const cfloat3 centre = cf3(0.5f, 0.5f, 0.5f);
    std::set<int> directions;
    for (std::size_t q = 0; q < m.quad_count(); ++q) {
        const cfloat3 p0 = m.positions[m.quads[q * 4 + 0]];
        const cfloat3 p1 = m.positions[m.quads[q * 4 + 1]];
        const cfloat3 p2 = m.positions[m.quads[q * 4 + 2]];
        const cfloat3 n = ccross(p1 - p0, p2 - p0);
        // The quad's own winding points away from the solid...
        CHECK(cdot(n, p0 - centre) > 0.0f);
        // ...and so does the first triangle of its triangulation, which is the
        // same three corners. The second triangle shares the diagonal.
        const cfloat3 t0 = m.positions[m.indices[q * 6 + 0]];
        const cfloat3 t1 = m.positions[m.indices[q * 6 + 1]];
        const cfloat3 t2 = m.positions[m.indices[q * 6 + 2]];
        CHECK(cdot(ccross(t1 - t0, t2 - t0), t0 - centre) > 0.0f);
        const int axis = cabs(n.x) > 0.5f ? 0 : (cabs(n.y) > 0.5f ? 1 : 2);
        const float sign = axis == 0 ? n.x : (axis == 1 ? n.y : n.z);
        directions.insert(axis * 2 + (sign > 0.0f ? 1 : 0));
    }
    CHECK(directions.size() == 6);  // all three axes, both signs
}

// The SECOND cause of non-manifoldness, which quad_mesh.h names beside the
// double-crossed cell because it is the one a voxel sculpt actually produces.
// Nothing here is a defect to fix — a diagonal solid has no manifold surface —
// but the header's claim about it is a claim, so it is measured.
TEST_CASE("diagonal occupancy is non-manifold in both modes, as the header says") {
    auto two_cells = [](VoxelCoord a, VoxelCoord b) {
        VoxelGrid g(0.1f);
        const std::uint8_t c = g.palette_add(cf3(1, 1, 1));
        g.fill_box(a, a, c);
        g.fill_box(b, b, c);
        return g;
    };
    auto over_used_edges = [](const Mesh& m) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
        for (std::size_t q = 0; q < m.quad_count(); ++q) {
            const std::uint32_t* c = &m.quads[q * 4];
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t a = c[k], b = c[(k + 1) % 4];
                uses[{std::min(a, b), std::max(a, b)}] += 1;
            }
        }
        std::map<int, std::size_t> histogram;
        for (const auto& [edge, n] : uses) histogram[n] += 1;
        return histogram;
    };

    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;

    SUBCASE("cells sharing only a lattice edge put one edge in four quads") {
        VoxelGrid g = two_cells({0, 0, 0}, {1, 1, 0});
        for (const Mesh& m : {g.mesh_quads(faces), g.mesh_quads()}) {
            REQUIRE(m.quad_count() == 12);
            const auto histogram = over_used_edges(m);
            CHECK(histogram.at(2) == 22);
            CHECK(histogram.at(4) == 1);  // the shared lattice edge
            CHECK(histogram.size() == 2);
        }
    }
    SUBCASE("cells sharing only a corner are edge-manifold and bowtied") {
        VoxelGrid g = two_cells({0, 0, 0}, {1, 1, 1});
        const Mesh m = g.mesh_quads(faces);
        REQUIRE(m.quad_count() == 12);
        const auto histogram = over_used_edges(m);
        CHECK(histogram.at(2) == 24);
        CHECK(histogram.size() == 1);  // no edge over two...
        // ...but the two cubes' 16 corners come back as 15 positions: the
        // shared one belongs to two sheets that meet nowhere else.
        CHECK(m.positions.size() == 15);
    }
    SUBCASE("a checkerboard is nothing but that case") {
        VoxelGrid g(0.1f);
        const std::uint8_t c = g.palette_add(cf3(1, 1, 1));
        for (int x = 0; x < 4; ++x)
            for (int y = 0; y < 4; ++y)
                for (int z = 0; z < 4; ++z)
                    if ((x + y + z) % 2 == 0) g.fill_box({x, y, z}, {x, y, z}, c);
        for (const Mesh& m : {g.mesh_quads(faces), g.mesh_quads()}) {
            REQUIRE(m.quad_count() == 192);
            CHECK(over_used_edges(m).at(4) == 108);
        }
    }
}

TEST_CASE("greedy meshing is untouched by faces mode existing") {
    VoxelGrid g = cube_grid(5);
    const Mesh greedy = g.mesh_greedy();
    // A 5^3 cube merges to one quad per side: two triangles, four vertices.
    CHECK(greedy.triangle_count() == 12);
    CHECK(greedy.positions.size() == 24);
    CHECK(greedy.normals.size() == 24);
    CHECK(greedy.quads.empty());

    std::vector<voxel::VoxelChunkMeshRange> ranges;
    const Mesh chunks = g.mesh_greedy_chunks(g.occupied_chunk_keys(), &ranges);
    CHECK(chunks.triangle_count() == 12);
    CHECK(chunks.quads.empty());
    CHECK(ranges.size() == 1);
}

TEST_CASE("decimation drops the quads it cannot keep") {
    scene::Tape t = sphere_tape(1.0f);
    const Mesh q = mesh::mesh_tape_quads(t, kSphereRegion, 0.1f);
    REQUIRE(q.has_quads());

    mesh::DecimateOptions options;
    options.target_ratio = 0.5f;
    const Mesh d = mesh::decimate(q, options);
    CHECK(d.quads.empty());
    CHECK(mesh::quads_consistent(d));
    // The triangles are what decimation would have produced without them.
    Mesh triangles_only = q;
    mesh::drop_quads(triangles_only);
    CHECK(mesh::decimate(triangles_only, options).indices == d.indices);
}

TEST_CASE("the consistency check catches a quad list that lies") {
    scene::Tape t = sphere_tape(1.0f);
    Mesh q = mesh::mesh_tape_quads(t, kSphereRegion, 0.2f);
    REQUIRE(mesh::quads_consistent(q));

    SUBCASE("a corner past the vertices") {
        q.quads[0] = static_cast<std::uint32_t>(q.positions.size());
        CHECK_FALSE(mesh::quads_consistent(q));
    }
    SUBCASE("a quad whose triangles are somebody else's") {
        std::swap(q.quads[1], q.quads[3]);
        CHECK_FALSE(mesh::quads_consistent(q));
    }
    SUBCASE("a count that does not match the triangles") {
        q.quads.resize(q.quads.size() - 4);
        CHECK_FALSE(mesh::quads_consistent(q));
    }
    SUBCASE("no quads at all is consistent") {
        mesh::drop_quads(q);
        CHECK(mesh::quads_consistent(q));
    }
}

// -- the count search --------------------------------------------------------

TEST_CASE("a target is approached and what was produced is reported") {
    scene::Tape t = sphere_tape(1.0f);
    mesh::QuadTarget want;
    want.target = 4000;
    mesh::QuadFit fit;
    const Mesh q = mesh::mesh_tape_quads_fit(t, kSphereRegion, 0.0f, want, {}, &fit);

    REQUIRE(fit.quad_count == q.quad_count());
    CHECK(mesh::quads_consistent(q));
    CHECK(fit.within_tolerance);
    CHECK_FALSE(fit.clamped);
    CHECK(fit.iterations > 0);
    CHECK(fit.iterations <= 4);
    CHECK(fit.cell_size > 0.0f);
    // The contract is the tolerance, not the exact number: assert the band the
    // header promises rather than a count that would re-baseline itself the
    // first time the lattice moves by a rounding.
    CHECK(std::abs(static_cast<double>(fit.quad_count) - 4000.0) <= 400.0);

    // And the report describes THIS mesh: meshing again at the cell size it
    // named reproduces the count it named.
    CHECK(mesh::mesh_tape_quads(t, kSphereRegion, fit.cell_size).quad_count() == fit.quad_count);
}

TEST_CASE("a shape with thin features is still landed on") {
    // Two thin plates joined by a thinner post: coarse lattices lose the post
    // entirely, so the count does not fall smoothly with cell size and a
    // search that assumed it did would step past its own answer.
    static scene::Document doc;
    doc = scene::Document{};
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::box(cf3(0.9f, 0.05f, 0.9f)), cf3(0, -0.5f, 0)));
    l.sdf->insert(item(scene::Prim::box(cf3(0.9f, 0.05f, 0.9f)), cf3(0, 0.5f, 0)));
    l.sdf->insert(item(scene::Prim::box(cf3(0.06f, 0.5f, 0.06f)), cf3(0, 0, 0)));
    scene::Tape t = scene::compile_document(doc);

    mesh::QuadTarget want;
    want.target = 6000;
    want.max_iterations = 6;
    mesh::QuadFit fit;
    const Mesh q = mesh::mesh_tape_quads_fit(t, {cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)},
                                             0.0f, want, {}, &fit);
    REQUIRE(q.quad_count() > 0);
    CHECK(fit.quad_count == q.quad_count());
    CHECK(fit.within_tolerance);
    CHECK(mesh::quads_consistent(q));
}

TEST_CASE("an explicit cell size skips the search") {
    scene::Tape t = sphere_tape(1.0f);
    mesh::QuadFit fit;
    const Mesh q = mesh::mesh_tape_quads_fit(t, kSphereRegion, 0.12f, {}, {}, &fit);

    CHECK(fit.iterations == 0);
    CHECK(fit.cell_size == 0.12f);
    CHECK(fit.within_tolerance);  // nothing was asked for, so nothing was missed
    CHECK_FALSE(fit.clamped);
    CHECK(same_geometry(q, mesh::mesh_tape_quads(t, kSphereRegion, 0.12f)));
    CHECK(fit.quad_count == q.quad_count());
}

TEST_CASE("neither a cell size nor a target meshes nothing") {
    scene::Tape t = sphere_tape(1.0f);
    mesh::QuadFit fit;
    const Mesh q = mesh::mesh_tape_quads_fit(t, kSphereRegion, 0.0f, {}, {}, &fit);
    CHECK(q.empty());
    CHECK(fit.cell_size == 0.0f);
    CHECK(fit.quad_count == 0);
}

TEST_CASE("the search never asks for a lattice outside the limits it was given") {
    // Directly, against a mesher that only COUNTS, because the limit that
    // matters in the field is the sample ceiling — and meshing at it means a
    // gigabyte of lattice per run, which is not something to spend on every
    // test. The property is the same one either way: the cell size handed to
    // the mesher stays inside [min, max], so the real search can never ask
    // mesh_tape_quads for a lattice it would refuse and then read the refusal
    // as a shape that vanished.
    std::vector<float> asked;
    auto counting_mesher = [&asked](float cell) {
        asked.push_back(cell);
        Mesh m;  // area / cell^2 quads, as the real relationship goes
        const std::size_t quads = static_cast<std::size_t>(12.0 / (cell * cell));
        m.positions.resize(quads == 0 ? 0 : quads);
        m.quads.resize(quads * 4, 0);
        m.indices.resize(quads * 6, 0);
        return m;
    };

    mesh::QuadTarget want;
    want.target = 100000000;  // far more than the fine limit below can produce
    want.max_iterations = 8;
    const mesh::QuadFit fit = mesh::fit_quad_cell(counting_mesher, 1.0f, 0.05f, 4.0f, want,
                                                  nullptr);
    REQUIRE_FALSE(asked.empty());
    for (float cell : asked) {
        CHECK(cell >= 0.05f);
        CHECK(cell <= 4.0f);
    }
    CHECK(fit.clamped);
    CHECK_FALSE(fit.within_tolerance);
    CHECK(fit.cell_size == 0.05f);  // the finest it is allowed, not the one it wanted
    CHECK(fit.quad_count > 0);
    // And it stops there rather than spending the whole cap re-meshing the
    // limit it is already standing on.
    CHECK(fit.iterations < 8);
}

TEST_CASE("a target the fine limit cannot reach stops at the limit and says so") {
    scene::Tape t = sphere_tape(1.0f);
    mesh::QuadTarget want;
    want.target = 4000000;
    // The caller's own floor, which is the same clamp the sample ceiling
    // applies and reports through the same flag — and one that does not cost a
    // gigabyte of lattice to reach.
    want.min_cell_size = 0.05f;
    mesh::QuadFit fit;
    const Mesh q = mesh::mesh_tape_quads_fit(t, kSphereRegion, 0.0f, want, {}, &fit);

    REQUIRE(q.quad_count() > 0);  // the finest lattice it is allowed, not nothing
    CHECK(fit.clamped);
    CHECK_FALSE(fit.within_tolerance);
    CHECK(fit.cell_size == 0.05f);
    CHECK(fit.quad_count == q.quad_count());
    CHECK(fit.iterations <= 4);
    CHECK(mesh::quads_consistent(q));
}

TEST_CASE("a target so small the shape collapses reports the best real mesh") {
    scene::Tape t = sphere_tape(1.0f);
    mesh::QuadTarget want;
    want.target = 1;  // no lattice produces one quad; a coarse enough one produces none
    mesh::QuadFit fit;
    const Mesh q = mesh::mesh_tape_quads_fit(t, kSphereRegion, 0.0f, want, {}, &fit);

    CHECK_FALSE(fit.within_tolerance);
    CHECK(fit.quad_count > 0);  // the collapse is never the answer
    CHECK(fit.quad_count == q.quad_count());
    CHECK(mesh::quads_consistent(q));
}

TEST_CASE("a tolerance far below the granularity exhausts the cap rather than looping") {
    scene::Tape t = sphere_tape(1.0f);
    mesh::QuadTarget want;
    want.target = 4000;
    want.tolerance = 1e-6f;  // an exact count, which the lattice cannot produce
    want.max_iterations = 5;
    mesh::QuadFit fit;
    const Mesh q = mesh::mesh_tape_quads_fit(t, kSphereRegion, 0.0f, want, {}, &fit);

    CHECK(fit.iterations <= 5);
    CHECK_FALSE(fit.within_tolerance);
    CHECK(fit.quad_count > 0);
    // Still the best attempt, which is well inside the band the default
    // tolerance would have accepted.
    CHECK(std::abs(static_cast<double>(fit.quad_count) - 4000.0) <= 400.0);
    CHECK(fit.quad_count == q.quad_count());
}

TEST_CASE("the voxel dual reaches a target and clamps at the voxel size") {
    VoxelGrid g = cube_grid(24);
    VoxelGrid::QuadOptions dual;
    mesh::QuadFit fit;

    SUBCASE("a reachable target") {
        mesh::QuadTarget want;
        want.target = 900;
        const Mesh q = g.mesh_quads_fit(dual, want, &fit);
        REQUIRE(q.quad_count() > 0);
        CHECK(fit.quad_count == q.quad_count());
        CHECK(fit.within_tolerance);
        CHECK(fit.cell_size >= g.voxel_size());
        CHECK(mesh::quads_consistent(q));
    }
    SUBCASE("a target finer than the grid holds") {
        // The clamp mesh_quads already applies, reported instead of silent: a
        // finer lattice over a step field buys quads and no detail.
        mesh::QuadTarget want;
        want.target = 1000000;
        const Mesh q = g.mesh_quads_fit(dual, want, &fit);
        CHECK(fit.clamped);
        CHECK_FALSE(fit.within_tolerance);
        CHECK(fit.cell_size == g.voxel_size());
        CHECK(same_geometry(q, g.mesh_quads()));
    }
}

TEST_CASE("faces mode picks a LEVEL, and says which one it picked") {
    VoxelGrid g = cube_grid(16);
    REQUIRE(g.add_level() == 1);
    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;

    const Mesh coarse = g.mesh_quads(faces);
    VoxelGrid::QuadOptions fine_opts = faces;
    fine_opts.level = 1;
    const Mesh fine = g.mesh_quads(fine_opts);
    REQUIRE(fine.quad_count() > coarse.quad_count());

    // Ask for what the fine level holds and the fine level is what comes back,
    // vertex for vertex — the level is the lever, not a rounding of one.
    mesh::QuadTarget want;
    want.target = fine.quad_count();
    mesh::QuadFit fit;
    const Mesh q = g.mesh_quads_fit(faces, want, &fit);
    CHECK(fit.cell_size == g.level_voxel_size(1));
    CHECK(fit.quad_count == fine.quad_count());
    CHECK(same_geometry(q, fine));

    // And a target between two levels lands on the nearer one rather than
    // pretending a lattice exists between them.
    want.target = coarse.quad_count() + 1;
    const Mesh near_coarse = g.mesh_quads_fit(faces, want, &fit);
    CHECK(fit.cell_size == g.level_voxel_size(0));
    CHECK(same_geometry(near_coarse, coarse));
    CHECK(fit.iterations > 0);
}

TEST_CASE("faces mode on a one-level grid has no lever and reports the clamp") {
    VoxelGrid g = cube_grid(8);
    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;
    mesh::QuadTarget want;
    want.target = 10;  // far below what the only level yields
    mesh::QuadFit fit;
    const Mesh q = g.mesh_quads_fit(faces, want, &fit);

    CHECK(same_geometry(q, g.mesh_quads(faces)));
    CHECK(fit.clamped);
    CHECK_FALSE(fit.within_tolerance);
    CHECK(fit.cell_size == g.voxel_size());
}

// REGRESSION (review round 1): the faces search is the WALK its header
// describes and not a secant over a cell size that is rounded to a level
// afterwards. The old search never visited the stack in order, never tested
// for an overshoot, and set `clamped` from a cell-size limit — so a target
// that simply fell between two levels came back with `clamped` false and a
// target the stack could not reach came back with `clamped` false too, which
// is the same answer for two opposite situations.
TEST_CASE("the faces target walks the level stack, and says when it ran out") {
    VoxelGrid g = cube_grid(4);
    REQUIRE(g.add_level() == 1);
    REQUIRE(g.add_level() == 2);
    REQUIRE(g.add_level() == 3);
    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;

    std::vector<std::size_t> counts;
    for (std::size_t l = 0; l < g.level_count(); ++l) {
        VoxelGrid::QuadOptions at = faces;
        at.level = l;
        counts.push_back(g.mesh_quads(at).quad_count());
    }
    REQUIRE(counts.size() == 4);
    for (std::size_t l = 1; l < counts.size(); ++l) REQUIRE(counts[l] > counts[l - 1]);

    mesh::QuadFit fit;
    SUBCASE("a target between two levels brackets it and is not clamped") {
        // Nearer the coarse level of the pair, so that is what must come back:
        // the finer one was meshed, compared and rejected.
        mesh::QuadTarget want;
        want.target = counts[1] + (counts[2] - counts[1]) / 4;
        const Mesh q = g.mesh_quads_fit(faces, want, &fit);

        CHECK(fit.quad_count == counts[1]);
        CHECK(fit.cell_size == g.level_voxel_size(1));
        CHECK(fit.iterations == 3);  // levels 0, 1, 2 — it stops at the overshoot
        CHECK_FALSE(fit.clamped);    // the stack holds this target; no level is nearer
        CHECK_FALSE(fit.within_tolerance);
        CHECK(fit.quad_count == q.quad_count());
    }
    SUBCASE("a target below the coarsest level is the coarse end of the stack") {
        mesh::QuadTarget want;
        want.target = counts[0] / 2;
        const Mesh q = g.mesh_quads_fit(faces, want, &fit);

        CHECK(fit.quad_count == counts[0]);
        CHECK(fit.cell_size == g.level_voxel_size(0));
        CHECK(fit.iterations == 1);  // the first level already overshot
        CHECK(fit.clamped);
        CHECK_FALSE(fit.within_tolerance);
    }
    SUBCASE("a target above the finest level reaches the fine end of the stack") {
        mesh::QuadTarget want;
        want.target = counts.back() * 100;
        // The cost knob does NOT truncate the walk: a stack is its own bound,
        // and stopping at level 1 here would report a level as the best the
        // grid holds while two finer ones sat unmeshed.
        want.max_iterations = 1;
        const Mesh q = g.mesh_quads_fit(faces, want, &fit);

        CHECK(fit.quad_count == counts.back());
        CHECK(fit.cell_size == g.level_voxel_size(g.level_count() - 1));
        CHECK(fit.iterations == static_cast<int>(g.level_count()));
        CHECK(fit.clamped);
        CHECK_FALSE(fit.within_tolerance);
        VoxelGrid::QuadOptions finest = faces;
        finest.level = g.level_count() - 1;
        CHECK(same_geometry(q, g.mesh_quads(finest)));
    }
    SUBCASE("a target a level hits exactly stops there") {
        mesh::QuadTarget want;
        want.target = counts[2];
        const Mesh q = g.mesh_quads_fit(faces, want, &fit);

        CHECK(fit.quad_count == counts[2]);
        CHECK(fit.cell_size == g.level_voxel_size(2));
        CHECK(fit.iterations == 3);
        CHECK(fit.within_tolerance);
        CHECK_FALSE(fit.clamped);
    }
}

TEST_CASE("a fit with no target is the plain mesher with a report attached") {
    VoxelGrid g = cube_grid(10);
    VoxelGrid::QuadOptions dual;
    mesh::QuadFit fit;
    const Mesh q = g.mesh_quads_fit(dual, {}, &fit);
    CHECK(same_geometry(q, g.mesh_quads()));
    CHECK(fit.iterations == 0);
    CHECK(fit.cell_size == g.voxel_size());
    CHECK(fit.quad_count == q.quad_count());
}

// -- export ------------------------------------------------------------------

namespace {

Mesh two_quads() {
    // A flat 2x1 strip: small enough that the exported bytes can be read by
    // eye, which is what makes the format assertions below assertions rather
    // than round trips through our own reader.
    Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(2, 0, 0),
                   cf3(0, 1, 0), cf3(1, 1, 0), cf3(2, 1, 0)};
    m.quads = {0, 1, 4, 3, 1, 2, 5, 4};
    for (std::size_t q = 0; q < 2; ++q) {
        const std::uint32_t* c = &m.quads[q * 4];
        m.indices.insert(m.indices.end(), {c[0], c[1], c[2], c[0], c[2], c[3]});
    }
    return m;
}

// The PolygonVertexIndex array of the first geometry, decoded from the binary
// FBX itself: the node's name, then the 'i' array property — element count,
// encoding (this writer never compresses), byte length, then the int32s.
std::vector<std::int32_t> fbx_polygon_indices(const std::vector<std::uint8_t>& bytes) {
    const std::string name = "PolygonVertexIndex";
    const std::string text(bytes.begin(), bytes.end());
    const std::size_t at = text.find(name);
    if (at == std::string::npos) return {};
    std::size_t p = at + name.size();
    auto u32 = [&] {
        std::uint32_t v = 0;
        std::memcpy(&v, bytes.data() + p, 4);
        p += 4;
        return v;
    };
    if (bytes[p++] != 'i') return {};
    const std::uint32_t count = u32();
    if (u32() != 0) return {};  // compressed: this writer does not
    if (u32() != count * 4) return {};
    std::vector<std::int32_t> out;
    for (std::uint32_t i = 0; i < count; ++i) out.push_back(static_cast<std::int32_t>(u32()));
    return out;
}

std::size_t count_lines(const std::string& text, const std::string& prefix) {
    std::size_t n = 0;
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t end = text.find('\n', pos);
        const std::string line = text.substr(pos, end - pos);
        if (line.rfind(prefix, 0) == 0) ++n;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return n;
}

}  // namespace

TEST_CASE("OBJ writes four-corner faces for a quad mesh and three for a triangle one") {
    const Mesh q = two_quads();
    REQUIRE(mesh::quads_consistent(q));
    const std::string obj = io::save_obj(q);
    CHECK(obj.find("f 1 2 5 4\n") != std::string::npos);
    CHECK(obj.find("f 2 3 6 5\n") != std::string::npos);
    CHECK(count_lines(obj, "f ") == 2);

    Mesh triangles = q;
    mesh::drop_quads(triangles);
    const std::string tri_obj = io::save_obj(triangles);
    CHECK(tri_obj.find("f 1 2 5\n") != std::string::npos);
    CHECK(tri_obj.find("f 1 5 4\n") != std::string::npos);
    CHECK(count_lines(tri_obj, "f ") == 4);
}

TEST_CASE("an exported quad OBJ re-imports as its fan triangulation") {
    // The stated asymmetry: the readers still fan-triangulate, so a quad file
    // comes back as triangles. Asserted rather than assumed, because the
    // header promises it.
    const Mesh q = two_quads();
    Mesh back;
    REQUIRE(io::load_obj(io::save_obj(q), &back).ok());
    CHECK(back.quads.empty());
    CHECK(back.triangle_count() == 4);
    CHECK(back.indices == q.indices);
}

TEST_CASE("PLY counts quads as faces, in binary and in ascii") {
    const Mesh q = two_quads();
    const std::vector<std::uint8_t> ascii = io::save_ply(q, /*binary=*/false);
    const std::string text(ascii.begin(), ascii.end());
    CHECK(text.find("element face 2\n") != std::string::npos);
    CHECK(text.find("4 0 1 4 3\n") != std::string::npos);
    CHECK(text.find("4 1 2 5 4\n") != std::string::npos);

    const std::vector<std::uint8_t> binary = io::save_ply(q, /*binary=*/true);
    const std::string header(binary.begin(), binary.begin() + 200);
    CHECK(header.find("element face 2\n") != std::string::npos);
    // Each row is one count byte plus four int32.
    const std::size_t end_header = header.find("end_header\n") + 11;
    CHECK(binary.size() == end_header + 6 * 12 + 2 * (1 + 16));
    CHECK(binary[end_header + 6 * 12] == 4);

    Mesh triangles = q;
    mesh::drop_quads(triangles);
    const std::vector<std::uint8_t> tri = io::save_ply(triangles, /*binary=*/false);
    const std::string tri_text(tri.begin(), tri.end());
    CHECK(tri_text.find("element face 4\n") != std::string::npos);
    CHECK(tri_text.find("3 0 1 4\n") != std::string::npos);
}

TEST_CASE("FBX writes four indices per polygon with the complement end marker") {
    const Mesh q = two_quads();
    const std::vector<std::uint8_t> fbx = io::save_fbx(q);
    REQUIRE(fbx.size() > 0);

    // Read PolygonVertexIndex out of the bytes rather than out of a parser:
    // the corner ORDER and the end marker are the whole contract here, and a
    // reader that triangulates on import — ours, assimp's — cannot show either.
    const std::vector<std::int32_t> poly = fbx_polygon_indices(fbx);
    REQUIRE(poly.size() == 8);  // two quads, four corners each
    const std::vector<std::int32_t> expect = {0, 1, 4, ~3, 1, 2, 5, ~4};
    CHECK(poly == expect);
    // Every fourth entry is negative and every other one is not: that is what
    // marks a polygon's end, and it is what says these are quads and not
    // triangles to any FBX reader.
    for (std::size_t i = 0; i < poly.size(); ++i) CHECK((poly[i] < 0) == (i % 4 == 3));

    Mesh triangles = q;
    mesh::drop_quads(triangles);
    const std::vector<std::int32_t> tri_poly = fbx_polygon_indices(io::save_fbx(triangles));
    REQUIRE(tri_poly.size() == 12);
    for (std::size_t i = 0; i < tri_poly.size(); ++i) CHECK((tri_poly[i] < 0) == (i % 3 == 2));
    // Read it back through ufbx, which is what a DCC does: a four-corner
    // polygon comes back as two triangles over the same corners.
    Mesh back;
    REQUIRE(io::load_fbx(fbx.data(), fbx.size(), &back).ok());
    CHECK(back.triangle_count() == 4);
    CHECK(back.positions.size() >= 6);
}

TEST_CASE("GLB keeps triangulating, because glTF has no quads") {
    const Mesh q = two_quads();
    const std::vector<std::uint8_t> glb = io::save_glb(q);
    Mesh triangles = q;
    mesh::drop_quads(triangles);
    CHECK(glb == io::save_glb(triangles));
    const std::string json(glb.begin(), glb.end());
    CHECK(json.find("\"mode\":4") != std::string::npos);
}

TEST_CASE("quad sample files land on disk for the independent validators") {
    // CI runs assimp over these and the Khronos validator over the GLB. A
    // four-index polygon is structurally fine in all three formats but the
    // writers here are minimal, so an independent parser is the gate that
    // matters — a unit test only proves our own reader agrees with our own
    // writer.
    VoxelGrid g = cube_grid(4);
    VoxelGrid::QuadOptions faces;
    faces.mode = VoxelGrid::QuadOptions::Mode::Faces;
    const Mesh boxes = g.mesh_quads(faces);
    REQUIRE(boxes.has_quads());

    scene::Tape t = sphere_tape(1.0f);
    const Mesh dual = mesh::mesh_tape_quads(t, kSphereRegion, 0.25f);
    REQUIRE(dual.has_quads());

    const std::string base = "quad_sample";
    CHECK(io::save_obj_file(dual, base + ".obj").ok());
    CHECK(io::save_ply_file(dual, base + ".ply").ok());
    CHECK(io::save_fbx_file(dual, base + ".fbx").ok());
    CHECK(io::save_glb_file(dual, base + ".glb").ok());  // triangles, by design
    CHECK(io::save_obj_file(boxes, base + "_faces.obj").ok());
    CHECK(io::save_ply_file(boxes, base + "_faces.ply").ok());
    CHECK(io::save_fbx_file(boxes, base + "_faces.fbx").ok());
}

// -- the mesh stream ---------------------------------------------------------

TEST_CASE("quads survive the mesh stream, and a triangle mesh's bytes do not move") {
    const Mesh q = two_quads();
    const std::vector<std::uint8_t> bytes = io::save_mesh_stream(q);

    Mesh triangles = q;
    mesh::drop_quads(triangles);
    const std::vector<std::uint8_t> tri_bytes = io::save_mesh_stream(triangles);
    // The quad section is APPENDED: the triangle bytes are a prefix of the
    // quad bytes, which is exactly what makes an older reader skip it.
    CHECK(bytes.size() == tri_bytes.size() + 4 + 8 * 4);
    CHECK(std::equal(tri_bytes.begin(), tri_bytes.end(), bytes.begin()));

    Mesh back;
    REQUIRE(io::load_mesh_stream(bytes.data(), bytes.size(), &back).ok());
    CHECK(back.quads == q.quads);
    CHECK(back.indices == q.indices);
    CHECK(back.positions.size() == q.positions.size());
    CHECK(mesh::quads_consistent(back));

    // The bytes an older writer produced still load, as the triangles they are.
    Mesh old;
    REQUIRE(io::load_mesh_stream(tri_bytes.data(), tri_bytes.size(), &old).ok());
    CHECK(old.quads.empty());
    CHECK(old.indices == q.indices);
}

TEST_CASE("a quad list that lies is written as triangles, not as a refusable document") {
    // The writer's half of the invariant. A stale quad list is a bug upstream,
    // but serialising it would turn that bug into a document this library
    // cannot open — so the section is dropped and the triangles, which are
    // still correct, are all that ships.
    Mesh q = two_quads();
    std::swap(q.quads[1], q.quads[3]);
    REQUIRE_FALSE(mesh::quads_consistent(q));

    const std::vector<std::uint8_t> bytes = io::save_mesh_stream(q);
    Mesh triangles = q;
    mesh::drop_quads(triangles);
    CHECK(bytes == io::save_mesh_stream(triangles));

    Mesh back;
    REQUIRE(io::load_mesh_stream(bytes.data(), bytes.size(), &back).ok());
    CHECK(back.quads.empty());
    CHECK(back.indices == q.indices);
}

TEST_CASE("a quad mesh layer survives a document round trip") {
    // The stream test above proves the section; this proves the section is
    // reached through the container a host actually saves — and that the
    // canonical bit-identity mesh layers already promise survives a mesh
    // carrying one more index array.
    io::ClaySpaceDoc cs;
    scene::Layer& l = cs.document.add_sdf_layer("quads");
    l.kind = scene::LayerKind::Mesh;
    l.sdf.reset();
    const Mesh q = two_quads();
    cs.mesh_layers.emplace(l.id, q);

    const std::vector<std::uint8_t> bytes = io::save_clayspace(cs);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    CHECK(io::save_clayspace(back) == bytes);

    REQUIRE(back.mesh_layers.count(l.id) == 1);
    const Mesh& loaded = back.mesh_layers.at(l.id);
    CHECK(loaded.quads == q.quads);
    CHECK(loaded.indices == q.indices);
    CHECK(mesh::quads_consistent(loaded));
}

TEST_CASE("a corrupt quad section is refused rather than half-believed") {
    const Mesh q = two_quads();
    const std::vector<std::uint8_t> good = io::save_mesh_stream(q);

    SUBCASE("a count that does not fit the bytes") {
        std::vector<std::uint8_t> bad = good;
        bad[good.size() - 4 - 8 * 4] = 99;
        Mesh out;
        CHECK_FALSE(io::load_mesh_stream(bad.data(), bad.size(), &out).ok());
    }
    SUBCASE("a corner past the vertices") {
        std::vector<std::uint8_t> bad = good;
        bad[good.size() - 8 * 4] = 200;
        Mesh out;
        CHECK_FALSE(io::load_mesh_stream(bad.data(), bad.size(), &out).ok());
    }
    SUBCASE("quads that are not the triangulation present") {
        std::vector<std::uint8_t> bad = good;
        bad[good.size() - 8 * 4] = 3;  // quad 0 corner 0: no longer triangle 0
        Mesh out;
        CHECK_FALSE(io::load_mesh_stream(bad.data(), bad.size(), &out).ok());
    }
    SUBCASE("a tail too short to hold even a count") {
        std::vector<std::uint8_t> bad(good.begin(), good.end() - 8 * 4 - 2);
        Mesh out;
        CHECK_FALSE(io::load_mesh_stream(bad.data(), bad.size(), &out).ok());
    }
}
