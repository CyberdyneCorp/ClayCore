#include <doctest/doctest.h>

#include "../../src/mesh/brick_recording.h"
#include <algorithm>
#include <array>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/scene/bounds.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/validate.h"
#include "clay/scene/tape.h"
#include "clay/parallel/thread_pool.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::gnarly_document;
using clay_test::item;
using mesh::Mesh;
using mesh::ValidationReport;

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

}  // namespace

TEST_CASE("sphere mesh: watertight, manifold, outward, right volume/area/euler") {
    scene::Tape tape = sphere_tape(1.0f);
    Mesh m = mesh::mesh_tape(tape, math::Aabb{cf3(-1.3f, -1.3f, -1.3f), cf3(1.3f, 1.3f, 1.3f)},
                             0.05f);
    REQUIRE(!m.empty());
    ValidationReport r = mesh::validate(m, 20000);
    CHECK(r.watertight);
    CHECK(r.manifold);
    CHECK(r.oriented);
    CHECK(r.degenerate_triangles == 0);
    CHECK(r.intersecting_pairs == 0);
    CHECK(r.euler_characteristic == 2);  // sphere topology
    CHECK(mesh::signed_volume(m) == doctest::Approx(4.18879).epsilon(0.02));   // outward normals
    CHECK(mesh::surface_area(m) == doctest::Approx(12.56637).epsilon(0.02));
}

TEST_CASE("torus mesh: euler characteristic 0 (genus 1)") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::torus(1.0f, 0.3f), cf3(0, 0, 0)));
    scene::Tape tape = scene::compile_document(doc);
    Mesh m = mesh::mesh_tape(tape, math::Aabb{cf3(-1.5f, -0.5f, -1.5f), cf3(1.5f, 0.5f, 1.5f)},
                             0.04f);
    ValidationReport r = mesh::validate(m);
    CHECK(r.watertight);
    CHECK(r.manifold);
    CHECK(r.euler_characteristic == 0);
}

TEST_CASE("golden gates: op x blend matrix meshes are clean") {
    using scene::Blend;
    using scene::BlendProfile;
    using scene::Op;
    const Op ops[] = {Op::Add, Op::Subtract, Op::Intersect};
    const BlendProfile profiles[] = {BlendProfile::Hard, BlendProfile::Quadratic,
                                     BlendProfile::Cubic, BlendProfile::Circular,
                                     BlendProfile::Chamfer};
    for (Op op : ops) {
        for (BlendProfile profile : profiles) {
            CAPTURE(static_cast<int>(op));
            CAPTURE(static_cast<int>(profile));
            scene::Document doc;
            scene::Layer& l = doc.add_sdf_layer("l");
            l.sdf->insert(item(scene::Prim::sphere(0.7f), cf3(-0.25f, 0, 0)));
            l.sdf->insert(item(scene::Prim::box(cf3(0.5f, 0.45f, 0.55f)), cf3(0.35f, 0.15f, 0),
                               op, Blend{profile, profile == BlendProfile::Hard ? 0.0f : 0.12f}));
            scene::Tape tape = scene::compile_document(doc);
            Mesh m = mesh::mesh_tape(
                tape, math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)}, 0.06f);
            REQUIRE(!m.empty());
            ValidationReport r = mesh::validate(m, 5000);
            CHECK(r.watertight);
            CHECK(r.manifold);
            CHECK(r.oriented);
            CHECK(r.degenerate_triangles == 0);
            CHECK(r.intersecting_pairs == 0);
        }
    }
}

TEST_CASE("golden gate: the gnarly composed scene meshes clean") {
    scene::Document doc = gnarly_document();
    scene::Tape tape = scene::compile_document(doc);
    Mesh m = mesh::mesh_tape(tape, math::Aabb{cf3(-2.5f, -2.5f, -2.5f), cf3(4.2f, 2.5f, 2.5f)},
                             0.08f);
    REQUIRE(!m.empty());
    ValidationReport r = mesh::validate(m);
    CHECK(r.watertight);
    CHECK(r.manifold);
    CHECK(r.oriented);
    CHECK(r.degenerate_triangles == 0);
}

TEST_CASE("brick-cache meshing is watertight across brick seams") {
    scene::Document doc = gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    brick::BrickCache cache(brick::BrickConfig{8, 0.08f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }
    Mesh m = mesh::mesh_bricks(cache, &doc);
    REQUIRE(!m.empty());
    ValidationReport r = mesh::validate(m);
    CHECK(r.watertight);  // no holes at brick boundaries
    CHECK(r.manifold);
    CHECK(r.oriented);
}

TEST_CASE("brick-cache meshing stays watertight where the field is steeper than the band") {
    // Issue #292: pinholes in the frame mesh of a worked surface, and none in
    // clay_document_mesh of the same document at the same moment.
    //
    // A cell is owned by the brick its LOW corner falls in, and takes its other
    // seven corners from up to seven neighbours. Marching only the cells owned
    // by surface bricks therefore misses a crossing whenever a brick that
    // stores no lattice — every sample a band or more from the surface, so
    // uniformly inside or uniformly outside — has a neighbour whose adjacent
    // sample has the opposite sign. That needs the field to move more than a
    // band over one voxel, which no 1-Lipschitz distance field does and a
    // worked document does routinely: relief and incise displace the surface by
    // an amplitude over a region narrower than the amplitude, and the tape says
    // so — safe_step_scale here is ~1e-8.
    //
    // The dabs below are the reported reproduction in miniature: a ball, a ring
    // of relief dabs with every third one incised.
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    scene::Node ball;
    ball.prim = scene::Prim::sphere(1.0f);
    layer.sdf->insert(std::move(ball));
    for (int i = 0; i < 40; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 40.0f;
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.14f);
        dab.xform.position = cf3(0.85f * std::cos(a), 0.85f * std::sin(a), 0.9f);
        dab.op = (i % 3 == 0) ? scene::Op::Incise : scene::Op::Relief;
        dab.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.9f};
        layer.sdf->insert(std::move(dab));
    }
    REQUIRE(scene::compile_document(doc).safe_step_scale() < 1.0f);

    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    brick::BrickCache cache(brick::BrickConfig{8, 0.05f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }
    std::vector<brick::BrickKey> surface = cache.surface_bricks();
    REQUIRE(surface.size() > 16);

    // The export path: every brick the cache stores, in one call. 28 boundary
    // edges before the fix.
    const Mesh whole = mesh::mesh_bricks(cache, nullptr, {});
    REQUIRE(!whole.empty());
    ValidationReport r = mesh::validate(whole);
    CHECK(r.boundary_edges == 0);
    CHECK(r.watertight);
    CHECK(r.manifold);
    CHECK(r.oriented);

    // And the frame path over the same bricks, which is what a sculptor looks
    // at: naming the keys must not open what naming none of them closes.
    const Mesh named = mesh::mesh_bricks(cache, nullptr, {}, &surface);
    ValidationReport rn = mesh::validate(named);
    CHECK(rn.boundary_edges == 0);
    CHECK(rn.watertight);
    CHECK(rn.manifold);
    CHECK(rn.oriented);
}

TEST_CASE("parallel brick meshing is deterministic and welds across seams") {
    // mesh_bricks marches bricks concurrently and welds them through ONE
    // Builder afterwards. The welding is what makes the sparse set watertight,
    // and it is exactly the shared mutable state a naive per-brick fan-out
    // would shard — so what is worth asserting is that the result is still one
    // welded mesh, and that it is the SAME mesh every run.
    scene::Document doc = gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    brick::BrickCache cache(brick::BrickConfig{8, 0.08f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }
    // Enough bricks that the pool actually splits the work; one brick would
    // take the single-chunk path and prove nothing about the fan-out.
    REQUIRE(cache.surface_bricks().size() > 16);

    std::vector<mesh::BrickMeshRange> ranges_a, ranges_b;
    Mesh a = mesh::mesh_bricks(cache, nullptr, {}, nullptr, &ranges_a);
    REQUIRE(!a.empty());

    // Byte-identical across runs. A race in the march, or a weld order that
    // depended on which thread got there first, shows up here.
    for (int run = 0; run < 8; ++run) {
        ranges_b.clear();
        Mesh b = mesh::mesh_bricks(cache, nullptr, {}, nullptr, &ranges_b);
        REQUIRE(b.positions.size() == a.positions.size());
        REQUIRE(b.indices.size() == a.indices.size());
        CHECK(std::memcmp(b.positions.data(), a.positions.data(),
                          a.positions.size() * sizeof(kernel::cfloat3)) == 0);
        CHECK(std::memcmp(b.indices.data(), a.indices.data(),
                          a.indices.size() * sizeof(std::uint32_t)) == 0);
        REQUIRE(ranges_b.size() == ranges_a.size());
        CHECK(std::memcmp(ranges_b.data(), ranges_a.data(),
                          ranges_a.size() * sizeof(mesh::BrickMeshRange)) == 0);
    }

    // And it is still ONE welded mesh rather than per-brick shells that happen
    // to touch: sharding the vertex map would duplicate every seam vertex and
    // this is what would fail.
    ValidationReport r = mesh::validate(a);
    CHECK(r.watertight);
    CHECK(r.manifold);
    CHECK(r.oriented);

    // The ranges still partition the mesh, which the subset path relies on.
    std::size_t vertices = 0, indices = 0;
    for (const mesh::BrickMeshRange& range : ranges_a) {
        vertices += range.vertex_count;
        indices += range.index_count;
    }
    CHECK(vertices == a.positions.size());
    CHECK(indices == a.indices.size());
}

TEST_CASE("brick meshing from inside a pooled loop still welds") {
    // mesh_bricks now dispatches, so a caller that is ALREADY inside a
    // parallel_for makes it a nested dispatch — which the pool runs inline.
    // The result must be the same mesh, which is what says the nesting guard
    // and this fan-out compose rather than merely coexist.
    scene::Document doc = gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    brick::BrickCache cache(brick::BrickConfig{8, 0.08f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }
    const Mesh direct = mesh::mesh_bricks(cache, nullptr, {});
    REQUIRE(!direct.empty());

    std::vector<Mesh> nested(4);
    clay::parallel::for_range(nested.size(), 1, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) nested[i] = mesh::mesh_bricks(cache, nullptr, {});
    });
    for (const Mesh& m : nested) {
        REQUIRE(m.positions.size() == direct.positions.size());
        CHECK(std::memcmp(m.positions.data(), direct.positions.data(),
                          direct.positions.size() * sizeof(kernel::cfloat3)) == 0);
        CHECK(std::memcmp(m.indices.data(), direct.indices.data(),
                          direct.indices.size() * sizeof(std::uint32_t)) == 0);
    }
}

TEST_CASE("vertex attributes: blend-faithful colors and gradient normals") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node a = item(scene::Prim::sphere(0.6f), cf3(-0.45f, 0, 0));
    a.color = cf3(1, 0, 0);
    l.sdf->insert(a);
    scene::Node b = item(scene::Prim::sphere(0.6f), cf3(0.45f, 0, 0), scene::Op::Add,
                         scene::Blend{scene::BlendProfile::Quadratic, 0.25f});
    b.color = cf3(0, 0, 1);
    l.sdf->insert(b);
    scene::Tape tape = scene::compile_document(doc);
    Mesh m = mesh::mesh_tape(tape, math::Aabb{cf3(-1.3f, -1.0f, -1.0f), cf3(1.3f, 1.0f, 1.0f)},
                             0.05f);
    REQUIRE(m.colors.size() == m.positions.size());
    REQUIRE(m.normals.size() == m.positions.size());

    bool found_left = false, found_right = false, found_mid = false;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        cfloat3 p = m.positions[i];
        cfloat3 c = m.colors[i];
        if (p.x < -0.7f && c.x > 0.9f && c.z < 0.1f) found_left = true;
        if (p.x > 0.7f && c.z > 0.9f && c.x < 0.1f) found_right = true;
        // near the joint: an actual gradient, neither pure red nor pure blue
        if (cabs(p.x) < 0.05f && c.x > 0.2f && c.x < 0.8f && c.z > 0.2f && c.z < 0.8f)
            found_mid = true;
        CHECK(clength(m.normals[i]) == doctest::Approx(1.0f).epsilon(1e-3));
    }
    CHECK(found_left);
    CHECK(found_right);
    CHECK(found_mid);

    // face normals + box UVs also work
    mesh::compute_face_normals(m);
    mesh::uv_box_project(m, 1.0f);
    CHECK(m.uvs.size() == m.positions.size());
}

TEST_CASE("gradient normals agree with geometry on a sphere") {
    scene::Tape tape = sphere_tape(1.0f);
    Mesh m = mesh::mesh_tape(tape, math::Aabb{cf3(-1.3f, -1.3f, -1.3f), cf3(1.3f, 1.3f, 1.3f)},
                             0.08f);
    for (std::size_t i = 0; i < m.positions.size(); i += 7) {
        cfloat3 radial = cnormalize(m.positions[i]);
        CHECK(cdot(m.normals[i], radial) > 0.999f);
    }
}

TEST_CASE("decimation: ratio target, watertightness, color boundaries") {
    scene::Tape tape = sphere_tape(1.0f);
    Mesh m = mesh::mesh_tape(tape, math::Aabb{cf3(-1.3f, -1.3f, -1.3f), cf3(1.3f, 1.3f, 1.3f)},
                             0.05f);
    std::size_t before = m.triangle_count();

    mesh::DecimateOptions opts;
    opts.target_ratio = 0.25f;
    opts.target_error = 0.05f;
    Mesh d = mesh::decimate(m, opts);
    CHECK(d.triangle_count() < before / 2);       // meaningful reduction
    CHECK(d.colors.size() == d.positions.size()); // attributes survive
    ValidationReport r = mesh::validate(d);
    CHECK(r.watertight);
    CHECK(r.manifold);
    CHECK(mesh::signed_volume(d) == doctest::Approx(4.18879).epsilon(0.1));
}

TEST_CASE("backend mesh(): CPU produces valid geometry; GPU matches topology") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.8f), cf3(0, 0, 0)));
    l.sdf->insert(item(scene::Prim::box(cf3(0.4f, 0.4f, 0.4f)), cf3(0.7f, 0, 0), scene::Op::Add,
                       scene::Blend{scene::BlendProfile::Quadratic, 0.1f}));
    scene::Tape tape = scene::compile_document(doc);

    eval::GridQuery grid;
    grid.origin = cf3(-1.4f, -1.4f, -1.4f);
    grid.spacing = 0.07f;
    grid.nx = grid.ny = grid.nz = 42;

    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu);
    std::vector<float> cpu_verts;
    std::vector<std::uint32_t> cpu_idx;
    REQUIRE(cpu->mesh(tape, grid, &cpu_verts, &cpu_idx) == eval::Status::Ok);
    Mesh cm;
    for (std::size_t i = 0; i < cpu_verts.size(); i += 3)
        cm.positions.push_back(cf3(cpu_verts[i], cpu_verts[i + 1], cpu_verts[i + 2]));
    cm.indices = cpu_idx;
    ValidationReport cr = mesh::validate(cm);
    CHECK(cr.watertight);
    CHECK(cr.manifold);
    CHECK(cr.oriented);

    for (eval::Backend* backend : eval::Registry::instance().all()) {
        if (backend == cpu) continue;
        CAPTURE(backend->name());
        std::vector<float> verts;
        std::vector<std::uint32_t> idx;
        eval::Status s = backend->mesh(tape, grid, &verts, &idx);
        if (s == eval::Status::Unsupported) continue;
        REQUIRE(s == eval::Status::Ok);
        Mesh gm;
        for (std::size_t i = 0; i < verts.size(); i += 3)
            gm.positions.push_back(cf3(verts[i], verts[i + 1], verts[i + 2]));
        gm.indices = idx;
        ValidationReport gr = mesh::validate(gm);
        // topology-invariant parity (meshing spec): watertight/manifold and
        // identical Euler characteristic, not bit-identical vertices
        CHECK(gr.watertight);
        CHECK(gr.manifold);
        CHECK(gr.euler_characteristic == cr.euler_characteristic);
    }
}

TEST_CASE("validator catches a hole and non-manifold fins") {
    scene::Tape tape = sphere_tape(0.8f);
    Mesh m = mesh::mesh_tape(tape, math::Aabb{cf3(-1.1f, -1.1f, -1.1f), cf3(1.1f, 1.1f, 1.1f)},
                             0.1f);
    REQUIRE(mesh::validate(m).watertight);

    // delete one triangle -> boundary edges appear
    Mesh holed = m;
    holed.indices.resize(holed.indices.size() - 3);
    ValidationReport r = mesh::validate(holed);
    CHECK_FALSE(r.watertight);
    CHECK(r.boundary_edges == 3);

    // duplicate a triangle -> non-manifold edges
    Mesh fin = m;
    fin.indices.push_back(fin.indices[0]);
    fin.indices.push_back(fin.indices[1]);
    fin.indices.push_back(fin.indices[2]);
    CHECK_FALSE(mesh::validate(fin).manifold);

    // degenerate triangle detected
    Mesh degen = m;
    degen.indices.push_back(0);
    degen.indices.push_back(0);
    degen.indices.push_back(1);
    CHECK(mesh::validate(degen).degenerate_triangles == 1);
}

TEST_CASE("the parallel lattice march welds seams exactly like the serial one") {
    // #119 calls vertex dedup across slab seams "the only genuinely fiddly
    // one". The answer is that slabs record WITHOUT welding and one Builder
    // replays them, so a vertex shared across a slab boundary is deduped by
    // the same code that deduped the serial march's repeated calls.
    //
    // Checked by building the serial mesh through the PUBLIC mesh_lattice over
    // the same evaluated grid that mesh_tape marches in parallel, and
    // requiring the two to be identical — vertex for vertex and index for
    // index, not merely the same triangle count.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node ball = clay_test::item(scene::Prim::sphere(0.45f), cf3(0, 0, 0));
    l.sdf->insert(ball);
    scene::Node cap = clay_test::item(scene::Prim::capsule(cf3(0, 0.1f, 0), cf3(0, 0.6f, 0), 0.16f),
                                      cf3(0, 0, 0));
    cap.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.1f};
    l.sdf->insert(cap);
    const scene::Tape tape = scene::compile_document(doc);
    const math::Aabb region{cf3(-0.6f, -0.6f, -0.6f), cf3(0.6f, 0.8f, 0.6f)};

    for (float cell : {0.05f, 0.02f, 0.012f}) {
        CAPTURE(cell);
        // The parallel path, through mesh_tape.
        const mesh::Mesh parallel = mesh::mesh_tape(tape, region, cell, {});

        // The serial reference: evaluate the same grid, march it through the
        // public (serial) mesh_lattice with the same out-of-range convention.
        const int nx = static_cast<int>(std::lround((region.max.x - region.min.x) / cell)) + 1;
        const int ny = static_cast<int>(std::lround((region.max.y - region.min.y) / cell)) + 1;
        const int nz = static_cast<int>(std::lround((region.max.z - region.min.z) / cell)) + 1;
        std::vector<float> values(static_cast<std::size_t>(nx) * ny * nz);
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    values[(static_cast<std::size_t>(k) * ny + j) * nx + i] =
                        tape.eval(region.min + cf3(static_cast<float>(i) * cell,
                                                   static_cast<float>(j) * cell,
                                                   static_cast<float>(k) * cell)).d;
        auto sample = [&](int i, int j, int k) -> float {
            if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return std::fabs(cell);
            return values[(static_cast<std::size_t>(k) * ny + j) * nx + i];
        };
        int cmin[3] = {-1, -1, -1};
        int cmax[3] = {nx, ny, nz};
        const mesh::Mesh serial = mesh::mesh_lattice(sample, cmin, cmax, region.min, cell);

        REQUIRE(parallel.positions.size() == serial.positions.size());
        REQUIRE(parallel.indices.size() == serial.indices.size());
        CHECK(parallel.indices == serial.indices);
        for (std::size_t v = 0; v < serial.positions.size(); ++v) {
            CAPTURE(v);
            REQUIRE(parallel.positions[v].x == serial.positions[v].x);
            REQUIRE(parallel.positions[v].y == serial.positions[v].y);
            REQUIRE(parallel.positions[v].z == serial.positions[v].z);
        }
        CHECK(serial.triangle_count() > 0);
    }
}

TEST_CASE("meshing a document gives the same mesh every time") {
    // A race in the slab march would show up as an answer that varies.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));
    const scene::Tape tape = scene::compile_document(doc);
    const math::Aabb region{cf3(-0.7f, -0.7f, -0.7f), cf3(0.7f, 0.7f, 0.7f)};

    const mesh::Mesh first = mesh::mesh_tape(tape, region, 0.011f, {});
    REQUIRE(first.triangle_count() > 0);
    for (int run = 0; run < 6; ++run) {
        const mesh::Mesh again = mesh::mesh_tape(tape, region, 0.011f, {});
        CAPTURE(run);
        REQUIRE(again.indices == first.indices);
        REQUIRE(again.positions.size() == first.positions.size());
        for (std::size_t v = 0; v < first.positions.size(); ++v)
            REQUIRE(again.positions[v].x == first.positions[v].x);
    }
}

TEST_CASE("the brick mesher emits no sliver triangles") {
    // A sliver is a near-zero-area triangle. Its face normal is a cross product
    // of near-parallel edges -- numerically garbage -- so it shades black
    // wherever gradient normals are unavailable, and a host re-meshed the WHOLE
    // FIELD after every completed stroke to hide them. That re-mesh evaluates
    // the field densely with no cull and measured 26.2 ms against the per-brick
    // path's 6.3 ms at 48 dabs, so the slivers were costing a host the cull
    // rather than merely a second pass (issue #549).
    //
    // They came from the crossing parameter landing at an edge endpoint, which
    // the cache's fp16 storage and band clamping make likely. Measured with
    // benchmarks/sliver_origin_probe.cpp before the fix: sliver vertices sat a
    // median of 0.0178 voxels from the lattice against 0.2576 for every other
    // vertex, which is what said a guard on `t` was the right fix rather than
    // a guess.
    //
    // THE GUARD IS ON THE BRICK PATH ONLY, so this case has to mesh through
    // mesh_bricks. An earlier version of it used mesh_tape and measured 234
    // slivers -- correct for that path, which is deliberately unguarded, and
    // no statement at all about this one.
    scene::Document doc = gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    brick::BrickCache cache(brick::BrickConfig{8, 0.08f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }
    Mesh m = mesh::mesh_bricks(cache, &doc);

    // A mesh of nothing has no slivers for the wrong reason.
    REQUIRE(m.triangle_count() > 1000);

    double longest = 0.0;
    std::vector<double> area2(m.triangle_count(), 0.0);
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const cfloat3& p0 = m.positions[m.indices[t * 3]];
        const cfloat3& p1 = m.positions[m.indices[t * 3 + 1]];
        const cfloat3& p2 = m.positions[m.indices[t * 3 + 2]];
        area2[t] = static_cast<double>(kernel::clength(kernel::ccross(p1 - p0, p2 - p0)));
        longest = std::max(longest, area2[t]);
    }
    REQUIRE(longest > 0.0);
    std::size_t slivers = 0;
    for (double a : area2)
        if (a < longest * 1e-3) ++slivers;
    CAPTURE(m.triangle_count());
    CHECK(slivers == 0);

    // And the guard did not cost the property the brick mesher already had:
    // the case index comes from SIGN TESTS, so `t` moves vertices without
    // changing which triangles exist.
    ValidationReport r = mesh::validate(m);
    CHECK(r.watertight);
    CHECK(r.manifold);
}

TEST_CASE("decimation recovers a pinch that is recoverable") {
    // meshoptimizer decides its own collapses and does not apply the link
    // condition `collapse_edge` refuses on, so a watertight 2-manifold input can
    // come back with edges carrying four incident triangles.
    //
    // THIS IS THE CONFIGURATION THAT DID IT, measured on an unmodified tree
    // before the fix: two tori crossed at a right angle, meshed at 0.035, and
    // decimated to a quarter. It produced two edges of incidence four -- each
    // with two forward and two backward triangles -- from an input with none,
    // and moved the Euler characteristic from -4 to -2. The collapse closed a
    // handle; the non-manifold edges are the scar (issue #567).
    //
    // The ratio matters as much as the shape. The same document is clean at
    // 0.05, 0.1, 0.4, 0.6, 0.8 and 0.9, so a case at any of those ratios would
    // pass without the fix and assert nothing.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::torus(0.7f, 0.28f), cf3(0, 0, 0)));
    scene::Node crossed = item(scene::Prim::torus(0.7f, 0.28f), cf3(0, 0, 0), scene::Op::Add,
                               scene::Blend{scene::BlendProfile::Quadratic, 0.1f});
    crossed.xform.rotation = math::Quat::from_axis_angle(cf3(1, 0, 0), 1.5707963f);
    l.sdf->insert(crossed);

    Mesh m = mesh::mesh_tape(scene::compile_document(doc),
                             math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)},
                             0.035f);
    ValidationReport in = mesh::validate(m);
    REQUIRE(in.triangles > 50000);  // the pinch needs the density it was found at
    REQUIRE(in.non_manifold_edges == 0);
    REQUIRE(in.watertight);

    mesh::DecimateOptions opts;
    opts.target_ratio = 0.25f;
    opts.target_error = 0.05f;
    Mesh d = mesh::decimate(m, opts);

    ValidationReport out = mesh::validate(d);
    CAPTURE(out.triangles);
    CHECK(out.non_manifold_edges == 0);
    CHECK(out.manifold);
    CHECK(out.watertight);

    // Recovered by asking again, not by giving up on decimating: a result that
    // returned the input would pass every check above and defeat the purpose.
    CHECK(d.triangle_count() < in.triangles / 2);

    // THE REPORT NEVER LIES, which is the part that holds on every toolchain.
    //
    // An earlier version asserted `report.attempts > 1` here -- that the first
    // simplification is the one that pinched. It does on AppleClang, across
    // twelve of the thirteen ratios from 0.21 to 0.32. It does NOT on the CI
    // Linux runner, where the first pass at this ratio comes back clean and no
    // retry runs. Which collapses meshoptimizer makes is decided by float
    // ordering inside its own queue, so WHETHER a given document pinches is a
    // property of the toolchain and not of this change. Pinning it failed CI on
    // Linux, and would have left the rest of this case asserting nothing there.
    mesh::DecimateReport report;
    Mesh again = mesh::decimate(m, opts, &report);
    CHECK(report.input_manifold);
    CHECK(report.manifold == (mesh::validate(again).non_manifold_edges == 0));
}

TEST_CASE("an unrecoverable pinch returns the requested size and says so") {
    // The counterpart, and the case CI found after the first version of this
    // fix shipped the opposite behaviour. At an aggressive ratio a pinch is not
    // an incidental bad collapse -- merging sheets is WHAT THE RATIO MEANS -- so
    // no retry recovers it.
    //
    // The first version returned the undecimated input here. On
    // examples/37_groups that turned a requested 12,418 triangles into 155,388
    // and blew the gallery's 400 KiB budget for committed models by tenfold.
    // A caller asking for a twelfth of the geometry is not served by all of it.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::torus(0.7f, 0.28f), cf3(0, 0, 0)));
    scene::Node crossed = item(scene::Prim::torus(0.7f, 0.28f), cf3(0, 0, 0), scene::Op::Add,
                               scene::Blend{scene::BlendProfile::Quadratic, 0.1f});
    crossed.xform.rotation = math::Quat::from_axis_angle(cf3(1, 0, 0), 1.5707963f);
    l.sdf->insert(crossed);
    Mesh m = mesh::mesh_tape(scene::compile_document(doc),
                             math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)},
                             0.035f);
    REQUIRE(mesh::validate(m).non_manifold_edges == 0);

    mesh::DecimateOptions opts;
    opts.target_ratio = 0.02f;  // one triangle in fifty
    opts.target_error = 0.5f;
    mesh::DecimateReport report;
    Mesh d = mesh::decimate(m, opts, &report);

    // Whatever the topology came out as, the result is DECIMATED -- that is the
    // property the gallery failure was about, and the regression it guards
    // returned 100% of the input. The bound is loose on purpose: exactly where
    // meshoptimizer stops is a toolchain-dependent float question, and pinning
    // that is the mistake the case above records.
    CHECK(d.triangle_count() < m.triangle_count() / 2);
    CHECK(report.input_manifold);
    CHECK(report.manifold == (mesh::validate(d).non_manifold_edges == 0));
}

TEST_CASE("decimation reports a pinch it was handed rather than claiming it") {
    // The deterministic half. Whether a DOCUMENT pinches under simplification
    // depends on the toolchain, so neither case above can be relied on to
    // exercise the reporting path on every platform. This one builds the
    // condition by hand and therefore fires everywhere: two quads sharing one
    // edge, folded so four triangles meet along it.
    Mesh m;
    m.positions = {cf3(0, 0, 0),  cf3(1, 0, 0),                      // the shared edge
                   cf3(0, 1, 0),  cf3(1, 1, 0),                      // sheet A
                   cf3(0, -1, 0), cf3(1, -1, 0),                     // sheet B
                   cf3(0, 0, 1),  cf3(1, 0, 1)};                     // sheet C, out of plane
    auto quad = [&m](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        m.indices.insert(m.indices.end(), {a, b, c, c, b, d});
    };
    quad(0, 1, 2, 3);
    quad(0, 1, 4, 5);
    quad(0, 1, 6, 7);
    REQUIRE(mesh::validate(m).non_manifold_edges > 0);  // the fixture is the point

    // Ratio 1.0 so the simplifier has nothing to remove: the result keeps the
    // pinch the fixture was built with, which is what makes this deterministic
    // rather than another bet on which collapses meshoptimizer picks.
    mesh::DecimateReport report;
    mesh::DecimateOptions opts;
    opts.target_ratio = 1.0f;
    Mesh d = mesh::decimate(m, opts, &report);
    REQUIRE(mesh::validate(d).non_manifold_edges > 0);
    CHECK_FALSE(report.manifold);
    CHECK_FALSE(report.input_manifold);
    // And it does not spend retries trying to clean up something it did not
    // break -- an input that arrives pinched is simplified once and returned.
    CHECK(report.attempts == 1);
}

namespace {

template <class T>
void check_recording_bytes(const std::vector<T>& a, const std::vector<T>& b) {
    REQUIRE(a.size() == b.size());
    if (!a.empty()) CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

void check_brick_recording(const brick::BrickCache& cache, const scene::Document& doc,
                           const std::vector<brick::BrickKey>* keys, int lod,
                           mesh::NormalMode normals) {
    mesh::MeshingOptions options{normals, lod == 0, 1e-4f};
    std::vector<mesh::BrickMeshRange> expected_ranges, actual_ranges;
    const auto expected = mesh::detail::mesh_bricks_recorded(
        cache, &doc, options, keys, &expected_ranges, nullptr, lod, false);
    const auto actual = mesh::mesh_bricks(cache, &doc, options, keys, &actual_ranges, nullptr, lod);
    check_recording_bytes(expected.positions, actual.positions);
    check_recording_bytes(expected.indices, actual.indices);
    check_recording_bytes(expected.normals, actual.normals);
    check_recording_bytes(expected.colors, actual.colors);
    check_recording_bytes(expected.uvs, actual.uvs);
    check_recording_bytes(expected_ranges, actual_ranges);
}

}  // namespace

TEST_CASE("brick-local edge recording preserves exact geometry attributes and ranges") {
    scene::Document doc;
    auto& layer = doc.add_sdf_layer("recording parity");
    auto sphere = item(scene::Prim::sphere(0.7f), cf3(-0.2f, 0.1f, 0.1f));
    sphere.color = cf3(0.2f, 0.6f, 0.9f);
    layer.sdf->insert(sphere);
    auto box = item(scene::Prim::box(cf3(0.4f, 0.6f, 0.15f)), cf3(0.2f, -0.2f, -0.3f));
    box.color = cf3(0.8f, 0.3f, 0.1f);
    layer.sdf->insert(box);
    auto* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    const auto tape = scene::compile_document(doc);

    // 8 and 16 are public configurations; 1 and 2 stress frequent seams, while
    // the internal cache at 32 exercises the bounded lookup's general fallback.
    for (int dim : {1, 2, 8, 16, 32}) {
        CAPTURE(dim);
        brick::BrickCache cache(brick::BrickConfig{dim, 0.1f, 3, 0});
        const float extent = std::max(1.6f, 0.2f * static_cast<float>(dim));
        cache.mark_dirty(math::Aabb{cf3(-extent, -extent, -extent), cf3(extent, extent, extent)});
        for (const auto& request : cache.take_dirty()) {
            std::vector<float> values(cache.config().sample_count());
            REQUIRE(cpu->eval_grid(tape, request.grid, values.data()) == eval::Status::Ok);
            cache.submit(request, values.data());
        }
        REQUIRE(!cache.surface_bricks().empty());
        if (dim == 8) {
            // Nested dispatch runs the inner march serially. Compare both
            // simultaneous results against ordinary unoptimized recording.
            std::array<Mesh, 2> nested;
            parallel::ThreadPool outer(2);
            outer.parallel_for(nested.size(), 1, [&](std::size_t first, std::size_t last) {
                for (std::size_t i = first; i < last; ++i)
                    nested[i] = mesh::mesh_bricks(cache, &doc);
            });
            const auto reference = mesh::detail::mesh_bricks_recorded(
                cache, &doc, {}, nullptr, nullptr, nullptr, 0, false);
            for (const auto& result : nested) {
                check_recording_bytes(reference.positions, result.positions);
                check_recording_bytes(reference.indices, result.indices);
                check_recording_bytes(reference.normals, result.normals);
                check_recording_bytes(reference.colors, result.colors);
            }
        }
        for (int x = -1; x <= 0; ++x)
            for (int y = -1; y <= 0; ++y)
                for (int z = -1; z <= 0; ++z)
                    REQUIRE(cache.build_mip({x, y, z}));

        for (int lod : {0, 1}) {
            CAPTURE(lod);
            auto keys = cache.surface_bricks_lod(lod);
            REQUIRE(!keys.empty());
            std::reverse(keys.begin(), keys.end());
            std::vector<brick::BrickKey> subset;
            for (std::size_t i = 0; i < keys.size(); i += 2) subset.push_back(keys[i]);
            auto repeated = subset;
            repeated.insert(repeated.end(), subset.begin(), subset.end());
            const std::vector<brick::BrickKey> empty;
            for (auto normals : {mesh::NormalMode::None, mesh::NormalMode::Face,
                                 mesh::NormalMode::Gradient}) {
                check_brick_recording(cache, doc, nullptr, lod, normals);
                check_brick_recording(cache, doc, &keys, lod, normals);
                check_brick_recording(cache, doc, &subset, lod, normals);
                check_brick_recording(cache, doc, &repeated, lod, normals);
                check_brick_recording(cache, doc, &empty, lod, normals);
            }
        }
    }
}
