#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "clay/brick/cache.h"
#include "clay/scene/bounds.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/validate.h"
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
