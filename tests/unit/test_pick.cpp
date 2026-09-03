#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "clay/pick/pick.h"
#include "clay/scene/bounds.h"
#include "clay/scene/cull_index.h"
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

TEST_CASE("a far high-Lipschitz item does not slow a ray that never nears it") {
    // A sphere with a ring of blended dabs, and a twisted box two units away.
    // The box's deformer bound (1 + k * r, about 3.5 here) is folded into the
    // whole tape's step scale, so a ray that never comes near the box used to
    // march at 0.28 of the step its own field allowed.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 0.785f;
        l.sdf->insert(item(scene::Prim::sphere(0.05f),
                           cf3(0.5f * std::cos(a), 0.5f * std::sin(a), 0), scene::Op::Add,
                           scene::Blend{scene::BlendProfile::Quadratic, 0.06f}));
    }
    scene::Node twisted = item(scene::Prim::box(cf3(0.1f, 0.1f, 0.3f)), cf3(2, 0, 0));
    twisted.deformers.push_back(scene::Deformer::twist(8.0f));
    const scene::NodeId twisted_id = l.sdf->insert(twisted);

    const scene::Tape whole = pick::pickable_tape(doc);
    REQUIRE(whole.safe_step_scale() < 1.0f);

    // Down the z axis onto the sphere's pole: the box is two units off to the
    // side of everything this ray evaluates.
    const math::Ray ray{cf3(0, 0, 3), cf3(0, 0, -1)};
    pick::RaycastOptions global;
    global.local_tape = false;
    const pick::SceneHit slow = pick::raycast_scene(doc, ray, global);
    const pick::SceneHit fast = pick::raycast_scene(doc, ray);
    REQUIRE(slow.hit);
    REQUIRE(fast.hit);
    // The same surface — only the step length changed — in fewer steps.
    CHECK(fast.t == doctest::Approx(2.5f).epsilon(1e-3));
    CHECK(fast.t == doctest::Approx(slow.t).epsilon(1e-3));
    CHECK(fast.layer == slow.layer);
    CHECK(fast.item == slow.item);
    CHECK(fast.steps < slow.steps);

    // The tape culled to the ray's segment is what bought that: the box is
    // dropped and its bound with it, so the local scale is the field's own.
    math::Aabb segment;
    segment.expand(ray.origin);
    segment.expand(ray.at(6.0f));
    const scene::CullRegion cull{segment.dilated(1e-3f)};
    CHECK(pick::pickable_tape(doc, &cull).safe_step_scale() == 1.0f);

    // The cached-tape entry point (what the C ABI calls) marches the same hit
    // with the same steps, with the document's cull index and without one:
    // the index is a pure acceleration of the culled compile.
    const scene::CullIndex index(doc);
    const pick::SceneHit cached = pick::raycast_scene(doc, whole, &index, ray);
    REQUIRE(cached.hit);
    CHECK(cached.t == fast.t);
    CHECK(cached.steps == fast.steps);
    CHECK(cached.item == fast.item);
    const pick::SceneHit unindexed = pick::raycast_scene(doc, whole, nullptr, ray);
    CHECK(unindexed.t == fast.t);
    CHECK(unindexed.steps == fast.steps);

    // A ray THROUGH the box keeps the box: the cull cannot drop it, the local
    // scale equals the whole tape's, and the march is the whole tape's own —
    // bit for bit, since equal scale means no local tape is used at all.
    const math::Ray through{cf3(2, 0, 3), cf3(0, 0, -1)};
    const pick::SceneHit box_slow = pick::raycast_scene(doc, through, global);
    const pick::SceneHit box_fast = pick::raycast_scene(doc, through);
    REQUIRE(box_slow.hit);
    REQUIRE(box_fast.hit);
    CHECK(box_fast.item == twisted_id);
    CHECK(box_fast.t == box_slow.t);
    CHECK(box_fast.steps == box_slow.steps);
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

// ---------------------------------------------------------------------------
// attribution without a Document per candidate
// ---------------------------------------------------------------------------
//
// pick::attribute used to answer "how far is the hit from THIS item's own
// surface" by building a Document holding a copy of the item and compiling
// it, once per candidate per pick, and compiled every layer's tape per call
// to choose the layer. It now emits the item alone (scene::compile_item) and
// reads a single-layer document's winner off the pickable tape it was
// marched on. The old implementation is kept HERE, verbatim, as the reference
// the new one is held against: the ids a hit attributes to must not move.

namespace {

float reference_item_field_distance(const scene::Layer& layer, const scene::Node& item,
                                    cfloat3 p) {
    scene::Document single;
    scene::Layer& l = single.add_sdf_layer("probe");
    l.xform = layer.xform;
    l.scale_axes = layer.scale_axes;
    l.mirror_axes = layer.mirror_axes;
    l.mirror_k = layer.mirror_k;
    scene::Node copy = item;
    copy.op = scene::Op::Add;
    copy.id = scene::kNoNode;
    copy.children.clear();
    l.sdf->insert(copy);
    scene::Tape t = scene::compile_document(single);
    return std::fabs(t.eval(p).d);
}

void reference_attribute_content(const scene::Layer& layer, const scene::SdfContent& content,
                                 const std::vector<scene::NodeId>& ids, cfloat3 p, float* best,
                                 scene::NodeId* best_item) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n || !n->visible) continue;
        if (n->is_group) {
            reference_attribute_content(layer, content, n->children, p, best, best_item);
            continue;
        }
        if (!scene::item_influence_bound(*n, layer).dilated(0.05f).contains(p)) continue;
        float d = reference_item_field_distance(layer, *n, p);
        if (d < *best) {
            *best = d;
            *best_item = id;
        }
    }
}

void reference_attribute(const scene::Document& doc, cfloat3 position, scene::LayerId* layer,
                         scene::NodeId* item) {
    *layer = 0;
    *item = scene::kNoNode;
    float best_layer_d = 3.4e38f;
    for (const scene::Layer& l : doc.layers) {
        if (!l.visible || l.ghost || l.kind != scene::LayerKind::Sdf || !l.sdf) continue;
        scene::Tape t = scene::compile_layer(l);
        if (t.empty()) continue;
        float d = std::fabs(t.eval(position).d);
        if (d < best_layer_d) {
            best_layer_d = d;
            *layer = l.id;
        }
    }
    const scene::Layer* winner = doc.find_layer(*layer);
    if (!winner || !winner->sdf) return;
    float best_item_d = 3.4e38f;
    reference_attribute_content(*winner, *winner->sdf, winner->sdf->roots, position,
                                &best_item_d, item);
}

// Several layers, each placed and scaled differently, one mirrored, one
// radial, one ghosted, one hidden; items with per-axis scale, a subtract, a
// paint, a group, a stroke. Everything attribution reads from a layer or an
// item, in one document.
scene::Document attribution_document() {
    scene::Document doc;
    scene::Layer& a = doc.add_sdf_layer("mirrored");
    a.xform.position = cf3(0.1f, 0.05f, 0);
    a.xform.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), 0.3f);
    a.mirror_axes = scene::kMirrorX;
    a.mirror_k = 0.03f;
    scene::Node side = item(scene::Prim::sphere(0.45f), cf3(0.6f, 0, 0));
    side.mirror = true;  // a copy on the far side of the plane
    a.sdf->insert(side);
    scene::Node squashed = item(scene::Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(0, 0.7f, 0),
                                scene::Op::Add, scene::Blend{scene::BlendProfile::Quadratic, 0.08f});
    squashed.scale_axes = cf3(0.5f, 1.0f, 2.0f);
    squashed.mirror = false;
    a.sdf->insert(squashed);
    a.sdf->insert(item(scene::Prim::capped_cylinder(0.2f, 0.5f), cf3(0.6f, 0.4f, 0),
                       scene::Op::Subtract, scene::Blend{scene::BlendProfile::Cubic, 0.04f}));
    scene::Node g;
    g.is_group = true;
    g.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.05f};
    scene::NodeId gid = a.sdf->insert(g);
    a.sdf->insert(item(scene::Prim::sphere(0.25f), cf3(0, -0.6f, 0.3f)), gid);
    a.sdf->insert(item(scene::Prim::torus(0.3f, 0.08f), cf3(0, -0.6f, -0.3f)), gid);
    a.sdf->insert(item(scene::Prim::sphere(0.3f), cf3(0.3f, 0.3f, 0.5f), scene::Op::Paint,
                       scene::Blend{scene::BlendProfile::Quadratic, 0.05f}));
    scene::Node stroke;
    stroke.prim = scene::Prim::stroke();
    stroke.stroke = {{cf3(-0.8f, 0.2f, 0.4f), 0.12f}, {cf3(-0.3f, 0.5f, 0.5f), 0.1f}};
    stroke.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.06f};
    a.sdf->insert(stroke);

    scene::Layer& b = doc.add_sdf_layer("squashed layer");
    b.scale_axes = cf3(1.5f, 1.0f, 0.75f);
    b.xform.position = cf3(0, -0.9f, 0);
    b.sdf->insert(item(scene::Prim::sphere(0.4f), cf3(0, 0, 0)));
    b.sdf->insert(item(scene::Prim::box(cf3(0.6f, 0.1f, 0.6f)), cf3(0, -0.5f, 0), scene::Op::Add,
                       scene::Blend{scene::BlendProfile::Quadratic, 0.1f}));

    scene::Layer& ghost = doc.add_sdf_layer("ghost");
    ghost.ghost = true;
    ghost.sdf->insert(item(scene::Prim::sphere(0.9f), cf3(0, 0, 0)));

    scene::Layer& hidden = doc.add_sdf_layer("hidden");
    hidden.visible = false;
    hidden.sdf->insert(item(scene::Prim::sphere(2.0f), cf3(0, 0, 0)));

    scene::Layer& radial = doc.add_sdf_layer("radial");
    radial.radial_count = 5;
    radial.radial_k = 0.02f;
    radial.xform.position = cf3(0, 1.4f, 0);
    scene::Node petal = item(scene::Prim::ellipsoid(cf3(0.25f, 0.1f, 0.15f)), cf3(0.6f, 0, 0));
    petal.mirror = true;
    radial.sdf->insert(petal);
    return doc;
}

// The harness's shape: one layer, a sphere and overlapping smooth dabs.
scene::Document dabs_document(int dabs) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("dabs");
    l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));
    for (int i = 0; i < dabs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(dabs);
        const float a = t * 6.2831853f * 3.0f;
        const float z = 1.0f - 2.0f * t;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z)) * 0.5f;
        l.sdf->insert(item(scene::Prim::sphere(0.05f), cf3(r * std::cos(a), r * std::sin(a), z * 0.5f),
                           scene::Op::Add, scene::Blend{scene::BlendProfile::Quadratic, 0.06f}));
    }
    return doc;
}

// A lattice over the document plus a deterministic scatter, so both the
// reject-everything far points and the crowded overlaps are covered.
std::vector<cfloat3> probe_points(float half, float step, int scatter, std::uint32_t seed) {
    std::vector<cfloat3> pts;
    for (float x = -half; x <= half + 1e-4f; x += step)
        for (float y = -half; y <= half + 1e-4f; y += step)
            for (float z = -half; z <= half + 1e-4f; z += step) pts.push_back(cf3(x, y, z));
    std::uint32_t s = seed;
    auto next = [&]() {
        s = s * 1664525u + 1013904223u;
        return (static_cast<float>(s >> 8) / 16777216.0f) * 2.0f * half - half;
    };
    for (int i = 0; i < scatter; ++i) {
        float x = next(), y = next(), z = next();
        pts.push_back(cf3(x, y, z));
    }
    return pts;
}

// Both entry points against the reference, at every point.
void check_attribution_matches_reference(const scene::Document& doc,
                                         const std::vector<cfloat3>& pts) {
    const scene::Tape tape = pick::pickable_tape(doc);
    int attributed = 0;
    for (const cfloat3& p : pts) {
        scene::LayerId want_layer, got_layer, got_layer_tape;
        scene::NodeId want_item, got_item, got_item_tape;
        reference_attribute(doc, p, &want_layer, &want_item);
        pick::attribute(doc, p, &got_layer, &got_item);
        pick::attribute(doc, tape, p, &got_layer_tape, &got_item_tape);
        CAPTURE(p.x);
        CAPTURE(p.y);
        CAPTURE(p.z);
        CHECK(got_layer == want_layer);
        CHECK(got_item == want_item);
        CHECK(got_layer_tape == want_layer);
        CHECK(got_item_tape == want_item);
        if (want_item != scene::kNoNode) ++attributed;
    }
    // The corpus has to reach items, or agreeing means nothing.
    CHECK(attributed > static_cast<int>(pts.size() / 8));
}

bool tapes_identical(const scene::Tape& a, const scene::Tape& b) {
    if (a.instrs.size() != b.instrs.size()) return false;
    for (std::size_t i = 0; i < a.instrs.size(); ++i)
        if (a.instrs[i].op != b.instrs[i].op ||
            a.instrs[i].param_offset != b.instrs[i].param_offset)
            return false;
    if (a.params != b.params || a.blob != b.blob) return false;
    if (a.info.is_exact != b.info.is_exact || a.info.lipschitz != b.info.lipschitz) return false;
    return a.bounds.min.x == b.bounds.min.x && a.bounds.min.y == b.bounds.min.y &&
           a.bounds.min.z == b.bounds.min.z && a.bounds.max.x == b.bounds.max.x &&
           a.bounds.max.y == b.bounds.max.y && a.bounds.max.z == b.bounds.max.z;
}

}  // namespace

TEST_CASE("compile_item is the single-item layer's compile, byte for byte") {
    // Every non-group item of both corpora, under its own layer with the
    // layer's full symmetry (mirror AND radial): compile_item must emit what a
    // layer holding only that item — as an Add, childless — compiles to.
    int compared = 0;
    for (const scene::Document& doc : {attribution_document(), gnarly_document()}) {
        for (const scene::Layer& layer : doc.layers) {
            if (!layer.sdf) continue;
            for (const auto& [id, node] : layer.sdf->nodes()) {
                if (node.is_group) continue;
                scene::Document single;
                scene::Layer& l = single.add_sdf_layer("single");
                const scene::LayerId keep = l.id;
                std::shared_ptr<scene::SdfContent> content = l.sdf;
                l = layer;  // placement, scale, mirror, radial, ghost, all of it
                l.id = keep;
                l.visible = true;
                l.sdf = content;
                scene::Node copy = node;
                copy.op = scene::Op::Add;
                copy.id = scene::kNoNode;
                copy.children.clear();
                l.sdf->insert(copy);
                const scene::Tape want = scene::compile_layer(l);
                const scene::Tape got = scene::compile_item(layer, node);
                CAPTURE(layer.name);
                CAPTURE(id);
                CHECK(tapes_identical(got, want));
                CHECK(!got.empty());
                ++compared;
            }
        }
    }
    CHECK(compared > 20);

    // A group has no field of its own.
    scene::Node g;
    g.is_group = true;
    CHECK(scene::compile_item(attribution_document().layers[0], g).empty());
}

TEST_CASE("attribution names the same layer and item as the per-candidate Document did") {
    SUBCASE("mirrored, squashed, ghosted, radial: several layers") {
        check_attribution_matches_reference(attribution_document(),
                                            probe_points(2.0f, 0.4f, 400, 7u));
    }
    SUBCASE("the gnarly corpus") {
        check_attribution_matches_reference(gnarly_document(), probe_points(2.0f, 0.5f, 300, 11u));
    }
    SUBCASE("one layer of overlapping dabs: the winner read off the pickable tape") {
        check_attribution_matches_reference(dabs_document(120), probe_points(0.8f, 0.2f, 300, 3u));
    }
    SUBCASE("one candidate layer beside a ghost: the pickable tape is the copy's") {
        scene::Document doc = dabs_document(20);
        scene::Layer& ghost = doc.add_sdf_layer("ghost");
        ghost.ghost = true;
        ghost.sdf->insert(item(scene::Prim::sphere(0.7f), cf3(0, 0, 0)));
        check_attribution_matches_reference(doc, probe_points(0.8f, 0.4f, 100, 5u));
    }
    SUBCASE("one layer whose tape is empty attributes nothing") {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("carve only");
        l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, 0), scene::Op::Subtract));
        const scene::Tape tape = pick::pickable_tape(doc);
        REQUIRE(tape.empty());
        scene::LayerId layer = 99;
        scene::NodeId node = 99;
        pick::attribute(doc, tape, cf3(0, 0, 0), &layer, &node);
        CHECK(layer == 0);
        CHECK(node == scene::kNoNode);
        pick::attribute(doc, cf3(0, 0, 0), &layer, &node);
        CHECK(layer == 0);
        CHECK(node == scene::kNoNode);
    }
    SUBCASE("hits attribute as the reference does at the hit point") {
        const scene::Document doc = attribution_document();
        const cfloat3 origins[] = {cf3(0, 0, -5), cf3(0, 0, 5),  cf3(-5, 0, 0),
                                   cf3(5, 0.3f, 0), cf3(0, 5, 0), cf3(0, -5, 0.2f),
                                   cf3(3, 3, 3),   cf3(-1.2f, 4, 0.4f)};
        int hits = 0;
        for (const cfloat3& o : origins) {
            const math::Ray ray{o, cnormalize(cf3(0, 0, 0) - o)};
            const pick::SceneHit h = pick::raycast_scene(doc, ray);
            if (!h.hit) continue;
            scene::LayerId layer;
            scene::NodeId node;
            reference_attribute(doc, h.position, &layer, &node);
            CHECK(h.layer == layer);
            CHECK(h.item == node);
            ++hits;
        }
        CHECK(hits >= 6);
    }
}
