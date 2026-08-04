#include <doctest/doctest.h>

#include <vector>

#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Every kernel primitive reachable from a document (add-primitive-backfill).

using namespace clay;
using namespace clay::kernel;
using clay_test::item;

namespace {

scene::Document one(scene::Prim prim) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(prim, cf3(0, 0, 0)));
    return doc;
}

}  // namespace

TEST_CASE("every backfilled primitive matches its kernel through the tape") {
    struct Case {
        const char* name;
        scene::Prim prim;
        float (*kernel_fn)(cfloat3);
    };
    const Case cases[] = {
        {"capped_torus", scene::Prim::capped_torus(1.0f, 0.9f, 0.25f),
         [](cfloat3 p) { return sd_capped_torus(p, cf2(csin(1.0f), ccos(1.0f)), 0.9f, 0.25f); }},
        {"link", scene::Prim::link(0.4f, 0.7f, 0.2f),
         [](cfloat3 p) { return sd_link(p, 0.4f, 0.7f, 0.2f); }},
        {"cylinder_infinite", scene::Prim::cylinder_infinite(0.1f, -0.2f, 0.5f),
         [](cfloat3 p) { return sd_cylinder_inf(p, cf2(0.1f, -0.2f), 0.5f); }},
        {"cone", scene::Prim::cone(0.6f, 1.2f),
         [](cfloat3 p) { return sd_cone(p, cf2(csin(0.6f), ccos(0.6f)), 1.2f); }},
        {"plane", scene::Prim::plane(cf3(0.3f, 1.0f, 0.0f), -0.2f),
         [](cfloat3 p) { return sd_plane(p, cnormalize(cf3(0.3f, 1.0f, 0.0f)), -0.2f); }},
        {"cut_sphere", scene::Prim::cut_sphere(1.0f, 0.35f),
         [](cfloat3 p) { return sd_cut_sphere(p, 1.0f, 0.35f); }},
        {"cut_hollow_sphere", scene::Prim::cut_hollow_sphere(1.0f, 0.3f, 0.08f),
         [](cfloat3 p) { return sd_cut_hollow_sphere(p, 1.0f, 0.3f, 0.08f); }},
        {"solid_angle", scene::Prim::solid_angle(0.8f, 1.0f),
         [](cfloat3 p) { return sd_solid_angle(p, cf2(csin(0.8f), ccos(0.8f)), 1.0f); }},
        {"tetrahedron", scene::Prim::tetrahedron(0.8f),
         [](cfloat3 p) { return sd_tetrahedron(p, 0.8f); }},
        {"dodecahedron", scene::Prim::dodecahedron(0.8f),
         [](cfloat3 p) { return sd_dodecahedron(p, 0.8f); }},
        {"icosahedron", scene::Prim::icosahedron(0.8f),
         [](cfloat3 p) { return sd_icosahedron(p, 0.8f); }},
        {"tri_prism", scene::Prim::tri_prism(0.7f, 0.4f),
         [](cfloat3 p) { return sd_tri_prism_bound(p, cf2(0.7f, 0.4f)); }},
        {"octahedron_cheap", scene::Prim::octahedron_cheap(0.9f),
         [](cfloat3 p) { return sd_octahedron_bound(p, 0.9f); }},
        {"lnorm_sphere", scene::Prim::lnorm_sphere(0.9f, 4.0f),
         [](cfloat3 p) { return sd_lnorm_sphere_bound(p, 0.9f, 4.0f); }},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        scene::Tape tape = scene::compile_document(one(c.prim));
        clay_test::Lcg rng(1501);
        for (int i = 0; i < 300; ++i) {
            cfloat3 p = rng.vec3(-3, 3);
            CHECK(tape.eval(p).d == doctest::Approx(c.kernel_fn(p)).epsilon(1e-5));
        }
    }
}

TEST_CASE("unbounded primitives report infinite influence and are never culled") {
    for (scene::Prim prim : {scene::Prim::plane(cf3(0, 1, 0), 0.0f),
                             scene::Prim::cylinder_infinite(0.0f, 0.0f, 0.4f)}) {
        scene::Document doc = one(prim);
        scene::Layer& l = doc.layers[0];
        CHECK(scene::item_influence_bound(*l.sdf->find(l.sdf->roots[0]), l).is_infinite());

        scene::Tape full = scene::compile_document(doc);
        const float band = 0.1f;
        clay_test::Lcg rng(1502);
        for (int b = 0; b < 15; ++b) {
            cfloat3 corner = rng.vec3(-30, 30);
            math::Aabb brick{corner, corner + cf3(0.3f, 0.3f, 0.3f)};
            scene::CullRegion cull{brick.dilated(band)};
            scene::Tape culled = scene::compile_document(doc, &cull);
            CHECK_FALSE(culled.empty());
            for (int i = 0; i < 40; ++i) {
                cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                                rng.range(brick.min.y, brick.max.y),
                                rng.range(brick.min.z, brick.max.z));
                CHECK(cclamp(full.eval(p).d, -band, band) ==
                      cclamp(culled.eval(p).d, -band, band));
            }
        }
    }
}

TEST_CASE("bound-only primitives downgrade tracked exactness and stay steppable") {
    for (scene::Prim prim : {scene::Prim::tri_prism(0.6f, 0.4f),
                             scene::Prim::octahedron_cheap(0.8f),
                             scene::Prim::lnorm_sphere(0.8f, 4.0f),
                             scene::Prim::ellipsoid(cf3(0.8f, 0.5f, 0.6f))}) {
        scene::Tape tape = scene::compile_document(one(prim));
        CHECK_FALSE(tape.info.is_exact);
        clay_test::check_conservative_steps([&](cfloat3 p) { return tape.eval(p).d; },
                                            tape.safe_step_scale(), 3.0f, 300, 1503);
    }
    // exact ones stay exact
    for (scene::Prim prim : {scene::Prim::tetrahedron(0.8f), scene::Prim::cut_sphere(1.0f, 0.2f),
                             scene::Prim::link(0.4f, 0.7f, 0.2f)}) {
        CHECK(scene::compile_document(one(prim)).info.is_exact);
    }
}

TEST_CASE("backfilled primitives keep their influence bounds") {
    const float band = 0.15f;
    const scene::Prim prims[] = {
        scene::Prim::capped_torus(1.0f, 0.7f, 0.2f), scene::Prim::link(0.3f, 0.5f, 0.15f),
        scene::Prim::cone(0.5f, 1.0f),               scene::Prim::cut_sphere(0.8f, 0.2f),
        scene::Prim::cut_hollow_sphere(0.8f, 0.2f, 0.06f),
        scene::Prim::solid_angle(0.7f, 0.9f),        scene::Prim::tetrahedron(0.7f),
        scene::Prim::dodecahedron(0.6f),             scene::Prim::icosahedron(0.6f),
        scene::Prim::tri_prism(0.6f, 0.4f),          scene::Prim::octahedron_cheap(0.7f),
        scene::Prim::lnorm_sphere(0.7f, 4.0f),
    };
    clay_test::Lcg rng(1504);
    for (const scene::Prim& prim : prims) {
        CAPTURE(static_cast<int>(prim.type));
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(scene::Prim::sphere(1.0f), cf3(0, 0, 0)));
        scene::NodeId id =
            l.sdf->insert(item(prim, cf3(3.2f, 0.3f, 0.2f), scene::Op::Add,
                               scene::Blend{scene::BlendProfile::Quadratic, 0.05f}));
        math::Aabb bound = scene::item_influence_bound(*l.sdf->find(id), l);
        REQUIRE_FALSE(bound.is_infinite());
        bound = bound.dilated(band);
        scene::Tape full = scene::compile_document(doc);
        l.sdf->remove(id);
        scene::Tape without = scene::compile_document(doc);

        int outside = 0;
        for (int i = 0; i < 2500; ++i) {
            cfloat3 p = rng.vec3(-8, 8);
            if (bound.contains(p)) continue;
            ++outside;
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(without.eval(p).d, -band, band));
        }
        CHECK(outside > 300);
    }
}

TEST_CASE("backfilled primitives round-trip and match the reference evaluator") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::icosahedron(0.7f), cf3(0, 0, 0)));
    l.sdf->insert(item(scene::Prim::link(0.3f, 0.6f, 0.15f), cf3(1.6f, 0, 0), scene::Op::Add,
                       scene::Blend{scene::BlendProfile::Quadratic, 0.08f}));
    l.sdf->insert(item(scene::Prim::cut_sphere(0.7f, 0.2f), cf3(-1.4f, 0.2f, 0),
                       scene::Op::Subtract, scene::Blend{scene::BlendProfile::Cubic, 0.05f}));

    scene::Tape tape = scene::compile_document(doc);
    clay_test::Lcg rng(1505);
    for (int i = 0; i < 400; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        CHECK(tape.eval(p).d ==
              doctest::Approx(clay_test::ref_eval_document(doc, p).d).epsilon(1e-5));
    }

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
    scene::Tape reloaded = scene::compile_document(*back);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        CHECK(tape.eval(p).d == reloaded.eval(p).d);
    }
}
