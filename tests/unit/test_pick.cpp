#include <doctest/doctest.h>

#include "clay/pick/pick.h"
#include "clay/scene/bounds.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::gnarly_document;
using clay_test::item;

namespace {

// two layers, two items each, spatially separated
scene::Document pick_document(scene::NodeId* sphere_id, scene::NodeId* box_id,
                              scene::LayerId* layer_a, scene::LayerId* layer_b) {
    scene::Document doc;
    scene::Layer& a = doc.add_sdf_layer("a");
    *layer_a = a.id;
    *sphere_id = a.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));
    a.sdf->insert(item(scene::Prim::capped_cylinder(0.2f, 0.4f), cf3(0, 1.2f, 0), scene::Op::Add,
                       scene::Blend{scene::BlendProfile::Quadratic, 0.05f}));
    scene::Layer& b = doc.add_sdf_layer("b");
    *layer_b = b.id;
    *box_id = b.sdf->insert(item(scene::Prim::box(cf3(0.4f, 0.4f, 0.4f)), cf3(3, 0, 0)));
    return doc;
}

}  // namespace

TEST_CASE("scene raycast attributes hits to layer and item") {
    scene::NodeId sphere_id, box_id;
    scene::LayerId layer_a, layer_b;
    scene::Document doc = pick_document(&sphere_id, &box_id, &layer_a, &layer_b);

    pick::SceneHit h1 = pick::raycast_scene(doc, {cf3(0, 0, -5), cf3(0, 0, 1)});
    REQUIRE(h1.hit);
    CHECK(h1.t == doctest::Approx(4.5f).epsilon(1e-3));
    CHECK(h1.layer == layer_a);
    CHECK(h1.item == sphere_id);
    CHECK(cdot(h1.normal, cf3(0, 0, -1)) > 0.99f);  // facing the ray

    pick::SceneHit h2 = pick::raycast_scene(doc, {cf3(3, 0, -5), cf3(0, 0, 1)});
    REQUIRE(h2.hit);
    CHECK(h2.layer == layer_b);
    CHECK(h2.item == box_id);

    // miss
    CHECK_FALSE(pick::raycast_scene(doc, {cf3(0, 5, -5), cf3(0, 0, 1)}).hit);
}

TEST_CASE("subtract items attribute their carved surfaces") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(1.0f), cf3(0, 0, 0)));
    scene::NodeId carve =
        l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, -1.0f), scene::Op::Subtract));
    pick::SceneHit h = pick::raycast_scene(doc, {cf3(0, 0, -5), cf3(0, 0, 1)});
    REQUIRE(h.hit);
    // the ray lands inside the carved bowl — that surface belongs to the cutter
    CHECK(h.item == carve);
}

TEST_CASE("brick raycast agrees with the analytic tape within a voxel") {
    scene::Document doc = gnarly_document();
    scene::Tape tape = scene::compile_document(doc);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    brick::BrickCache cache(brick::BrickConfig{8, 0.06f, 3, 0});
    cache.mark_dirty(tape.bounds);
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape t = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(t, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }

    clay_test::Lcg rng(801);
    int agreements = 0;
    for (int i = 0; i < 60; ++i) {
        math::Ray ray{cf3(rng.range(-1.5f, 1.5f), rng.range(-1.5f, 1.5f), -4.0f),
                      cnormalize(cf3(rng.range(-0.15f, 0.15f), rng.range(-0.15f, 0.15f), 1.0f))};
        pick::SceneHit analytic = pick::raycast_scene(doc, ray);
        pick::SceneHit bricks = pick::raycast_bricks(cache, ray);
        if (!analytic.hit) continue;
        REQUIRE(bricks.hit);
        CHECK(clength(bricks.position - analytic.position) < cache.config().voxel_size);
        ++agreements;
    }
    CHECK(agreements > 20);
}

TEST_CASE("surface snapping: position and normal from nearby points") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(1.0f), cf3(0, 0, 0)));
    scene::Tape tape = scene::compile_document(doc);

    clay_test::Lcg rng(802);
    for (int i = 0; i < 200; ++i) {
        cfloat3 dir = rng.unit3();
        cfloat3 p = dir * rng.range(0.85f, 1.15f);  // within ~2 voxels at res 512
        pick::SnapResult s = pick::snap_to_surface(tape, p);
        REQUIRE(s.ok);
        CHECK(cabs(tape.eval(s.position).d) < 1e-4f);
        CHECK(clength(s.position - dir) < 0.02f);      // snapped radially
        CHECK(cdot(s.normal, dir) > 0.999f);           // outward normal
    }
}

TEST_CASE("voxel raycast: cell, entry face, adjacent placement") {
    voxel::VoxelGrid grid(0.5f);
    std::uint8_t c = grid.palette_add(cf3(1, 1, 1));
    grid.fill_box({0, 0, 0}, {2, 2, 2}, c);

    // ray traveling -X hits the +X face of cell (2,1,1)
    pick::VoxelHit h = pick::raycast_voxels(grid, {cf3(5, 0.75f, 0.75f), cf3(-1, 0, 0)});
    REQUIRE(h.hit);
    CHECK(h.cell == voxel::VoxelCoord{2, 1, 1});
    CHECK(h.face == 0);  // +X entry face
    CHECK(pick::adjacent_cell(h) == voxel::VoxelCoord{3, 1, 1});

    // ray traveling +Z hits the -Z face of cell (1,1,0)
    pick::VoxelHit h2 = pick::raycast_voxels(grid, {cf3(0.75f, 0.75f, -3), cf3(0, 0, 1)});
    REQUIRE(h2.hit);
    CHECK(h2.cell == voxel::VoxelCoord{1, 1, 0});
    CHECK(h2.face == 5);  // -Z entry face
    CHECK(pick::adjacent_cell(h2) == voxel::VoxelCoord{1, 1, -1});

    // diagonal ray still lands on a solid cell
    pick::VoxelHit h3 =
        pick::raycast_voxels(grid, {cf3(3, 3, 3), cnormalize(cf3(-1, -1, -1))});
    REQUIRE(h3.hit);
    CHECK(grid.get(h3.cell) == c);
    // and its adjacent cell is empty (placement is valid)
    CHECK(grid.get(pick::adjacent_cell(h3)) == 0);

    // miss
    CHECK_FALSE(pick::raycast_voxels(grid, {cf3(5, 5, 5), cf3(1, 0, 0)}).hit);

    // build-plane pass-through
    auto bp = pick::pick_build_plane(grid, {cf3(0.6f, 4, 0.6f), cf3(0, -1, 0)}, 0);
    REQUIRE(bp.has_value());
    CHECK(*bp == voxel::VoxelCoord{1, 0, 1});
}

TEST_CASE("selection bounds: tight union for zoom-to-selection") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.xform.position = cf3(0, 1, 0);
    scene::NodeId a = l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(-1, 0, 0)));
    scene::NodeId b = l.sdf->insert(item(scene::Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(2, 0, 0),
                                         scene::Op::Add,
                                         scene::Blend{scene::BlendProfile::Quadratic, 0.4f}));

    math::Aabb sel = pick::selection_bounds(doc, l.id, {a, b});
    REQUIRE(!sel.empty());
    // contains both shapes (layer transform applied)
    CHECK(sel.min.x == doctest::Approx(-1.5f).epsilon(1e-4));
    CHECK(sel.max.x == doctest::Approx(2.3f).epsilon(1e-4));
    CHECK(sel.min.y == doctest::Approx(0.5f).epsilon(1e-4));
    // tight: no blend-support dilation (influence bound would add ~1.6)
    math::Aabb influence = scene::item_influence_bound(*l.sdf->find(b), l);
    CHECK(sel.max.x < influence.max.x - 0.5f);

    // single-node and layer bounds
    math::Aabb only_a = pick::selection_bounds(doc, l.id, {a});
    CHECK(only_a.max.x == doctest::Approx(-0.5f).epsilon(1e-4));
    CHECK(pick::layer_bounds(l).max.x == doctest::Approx(2.3f).epsilon(1e-4));
}

// Regression: pick's shape bounds computed prim_local_bounds directly, so a
// repeated or deformed item reported the extent of ONE undeformed copy. That
// makes zoom-to-selection frame the wrong region, and it silently disagreed
// with scene::item_geometry_bound, which had always applied both.
TEST_CASE("pick bounds cover repetition and deformers") {
    SUBCASE("finite grid spans every cell") {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n = item(scene::Prim::sphere(0.25f), cf3(0, 0, 0));
        n.repeat = scene::Repeat::grid_finite(0.9f, cf3(2, 1, 1));
        l.sdf->insert(n);

        math::Aabb b = pick::layer_bounds(l);
        // reach = spacing * counts, so x spans 2 cells either side
        CHECK(b.max.x == doctest::Approx(0.25f + 1.8f).epsilon(1e-4));
        CHECK(b.max.y == doctest::Approx(0.25f + 0.9f).epsilon(1e-4));
        CHECK(b.min.x == doctest::Approx(-(0.25f + 1.8f)).epsilon(1e-4));

        // and it must not exceed what the scene layer reports as geometry
        math::Aabb geom = scene::item_geometry_bound(*l.sdf->find(l.sdf->roots[0]), l);
        CHECK(b.max.x <= geom.max.x + 1e-4f);
    }

    SUBCASE("radial array sweeps the annulus") {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n = item(scene::Prim::box(cf3(0.2f, 0.3f, 0.2f)), cf3(0, 0, 0));
        n.repeat = scene::Repeat::radial(8, 1.0f);
        l.sdf->insert(n);

        math::Aabb b = pick::layer_bounds(l);
        CHECK(b.max.x > 1.0f);  // copies sit out at the offset radius
        CHECK(b.max.z > 1.0f);
        CHECK(b.max.y == doctest::Approx(0.3f).epsilon(1e-4));  // axis extent unchanged
    }

    SUBCASE("a twisted box is wider than the box") {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n = item(scene::Prim::box(cf3(0.3f, 1.0f, 0.3f)), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::twist(2.0f));
        l.sdf->insert(n);

        math::Aabb plain = scene::prim_local_bounds(n);
        math::Aabb b = pick::layer_bounds(l);
        CHECK(b.max.x >= plain.max.x);  // twisting sweeps the corners outward
    }

    SUBCASE("selection bounds agree with layer bounds for one repeated item") {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n = item(scene::Prim::sphere(0.2f), cf3(0, 0, 0));
        n.repeat = scene::Repeat::grid_finite(0.5f, cf3(1, 0, 0));
        scene::NodeId id = l.sdf->insert(n);

        math::Aabb sel = pick::selection_bounds(doc, l.id, {id});
        math::Aabb all = pick::layer_bounds(l);
        CHECK(sel.max.x == doctest::Approx(all.max.x).epsilon(1e-4));
        CHECK(sel.max.x == doctest::Approx(0.7f).epsilon(1e-4));
    }
}
