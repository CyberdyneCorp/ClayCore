#include <doctest/doctest.h>

#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Grid and radial arrays through the tape (add-repetition).

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using scene::Repeat;

namespace {

scene::Document repeat_doc(scene::Prim prim, const Repeat& repeat, float blend_k = 0.0f) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(prim, cf3(0, 0, 0), scene::Op::Add,
                         scene::Blend{blend_k > 0 ? scene::BlendProfile::Quadratic
                                                  : scene::BlendProfile::Hard,
                                      blend_k});
    n.repeat = repeat;
    l.sdf->insert(n);
    return doc;
}

}  // namespace

TEST_CASE("infinite grid repetition is periodic and matches the kernel") {
    scene::Tape tape = scene::compile_document(
        repeat_doc(scene::Prim::sphere(0.3f), Repeat::grid_infinite(cf3(2, 2, 2))));
    clay_test::Lcg rng(1401);
    for (int i = 0; i < 500; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        CHECK(tape.eval(p).d ==
              doctest::Approx(sd_sphere(crep_inf_point(p, cf3(2, 2, 2)), 0.3f)).epsilon(1e-5));
        // periodic under a full cell shift on every axis
        CHECK(tape.eval(p).d == doctest::Approx(tape.eval(p + cf3(2, 0, 0)).d).epsilon(1e-4));
        CHECK(tape.eval(p).d == doctest::Approx(tape.eval(p + cf3(0, -4, 2)).d).epsilon(1e-4));
    }
}

TEST_CASE("finite grid has copies only inside its extent") {
    // 5 x 3 x 1 cells of spacing 2
    scene::Tape tape = scene::compile_document(
        repeat_doc(scene::Prim::sphere(0.35f), Repeat::grid_finite(2.0f, cf3(2, 1, 0))));

    auto brute = [](cfloat3 p) {
        float best = 3.4e38f;
        for (int x = -2; x <= 2; ++x)
            for (int y = -1; y <= 1; ++y)
                best = cmin(best, sd_sphere(p - cf3(2.0f * (float)x, 2.0f * (float)y, 0.0f),
                                            0.35f));
        return best;
    };
    clay_test::Lcg rng(1402);
    for (int i = 0; i < 800; ++i) {
        cfloat3 p = rng.vec3(-8, 8);
        CHECK(tape.eval(p).d == doctest::Approx(brute(p)).epsilon(1e-4));
    }
    // no phantom copy one cell past the extent
    CHECK(tape.eval(cf3(6, 0, 0)).d > 0.3f);
    CHECK(tape.eval(cf3(0, 4, 0)).d > 0.3f);
    // and the outermost real copy is solid
    CHECK(tape.eval(cf3(4, 2, 0)).d < 0.0f);
}

TEST_CASE("radial array is periodic under sector rotation") {
    const int count = 6;
    scene::Tape tape = scene::compile_document(
        repeat_doc(scene::Prim::sphere(0.25f), Repeat::radial(count, 1.0f), 0.0f));
    // the item sits at the origin of its cell, so place it via the offset:
    // sample on the ring where copies live
    float sector = 6.2831853f / static_cast<float>(count);
    clay_test::Lcg rng(1403);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        float a = tape.eval(p).d;
        // rotate by one sector about Y
        float c = ccos(sector), s = csin(sector);
        cfloat3 q = cf3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
        CHECK(a == doctest::Approx(tape.eval(q).d).epsilon(2e-4));
    }
}

TEST_CASE("repetition matches the reference tree evaluator") {
    scene::Document doc = repeat_doc(scene::Prim::box(cf3(0.3f, 0.3f, 0.3f)),
                                     Repeat::grid_finite(1.5f, cf3(1, 1, 1)));
    scene::Tape tape = scene::compile_document(doc);
    clay_test::Lcg rng(1404);
    for (int i = 0; i < 500; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        CHECK(tape.eval(p).d ==
              doctest::Approx(clay_test::ref_eval_document(doc, p).d).epsilon(1e-5));
    }
}

TEST_CASE("half-cell condition drives tracked exactness") {
    // comfortably inside its half-cell: stays exact
    scene::Tape roomy = scene::compile_document(
        repeat_doc(scene::Prim::sphere(0.3f), Repeat::grid_finite(2.0f, cf3(1, 0, 0))));
    CHECK(roomy.info.is_exact);
    CHECK(roomy.safe_step_scale() == doctest::Approx(1.0f));

    // an item wider than its half-cell overflows: downgraded to a bound
    scene::Tape crowded = scene::compile_document(
        repeat_doc(scene::Prim::sphere(0.9f), Repeat::grid_finite(1.0f, cf3(1, 0, 0))));
    CHECK_FALSE(crowded.info.is_exact);

    // blend influence counts toward the budget too
    scene::Tape blended = scene::compile_document(
        repeat_doc(scene::Prim::sphere(0.3f), Repeat::grid_finite(1.0f, cf3(1, 0, 0)), 0.2f));
    CHECK_FALSE(blended.info.is_exact);

    // stepping stays safe in every case
    for (const scene::Tape* t : {&roomy, &crowded, &blended})
        clay_test::check_conservative_steps([&](cfloat3 p) { return t->eval(p).d; },
                                            t->safe_step_scale(), 3.0f, 300, 1405);
}

TEST_CASE("finite and radial arrays respect their influence bounds") {
    const float band = 0.15f;
    struct Case {
        const char* name;
        Repeat repeat;
    };
    const Case cases[] = {
        {"grid", Repeat::grid_finite(1.2f, cf3(2, 1, 0))},
        {"radial", Repeat::radial(5, 1.0f)},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(scene::Prim::sphere(1.0f), cf3(0, 0, 0)));
        scene::Node arr = item(scene::Prim::sphere(0.2f), cf3(4.0f, 0.5f, 0.0f), scene::Op::Add,
                               scene::Blend{scene::BlendProfile::Quadratic, 0.04f});
        arr.repeat = c.repeat;
        scene::NodeId id = l.sdf->insert(arr);

        math::Aabb bound = scene::item_influence_bound(*l.sdf->find(id), l);
        REQUIRE_FALSE(bound.is_infinite());
        bound = bound.dilated(band);
        scene::Tape full = scene::compile_document(doc);
        l.sdf->remove(id);
        scene::Tape without = scene::compile_document(doc);

        clay_test::Lcg rng(1406);
        int outside = 0;
        for (int i = 0; i < 4000; ++i) {
            cfloat3 p = rng.vec3(-9, 9);
            if (bound.contains(p)) continue;
            ++outside;
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(without.eval(p).d, -band, band));
        }
        CHECK(outside > 500);
    }
}

TEST_CASE("infinite grids report infinite influence and are never culled") {
    scene::Document doc = repeat_doc(scene::Prim::sphere(0.3f),
                                     Repeat::grid_infinite(cf3(2, 2, 2)));
    scene::Layer& l = doc.layers[0];
    CHECK(scene::item_influence_bound(*l.sdf->find(l.sdf->roots[0]), l).is_infinite());

    scene::Tape full = scene::compile_document(doc);
    const float band = 0.1f;
    clay_test::Lcg rng(1407);
    for (int b = 0; b < 20; ++b) {
        cfloat3 corner = rng.vec3(-20, 20);
        math::Aabb brick{corner, corner + cf3(0.3f, 0.3f, 0.3f)};
        scene::CullRegion cull{brick.dilated(band)};
        scene::Tape culled = scene::compile_document(doc, &cull);
        CHECK_FALSE(culled.empty());  // never dropped, however far away
        for (int i = 0; i < 60; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(culled.eval(p).d, -band, band));
        }
    }
}

TEST_CASE("repetition composes with deformers and round-trips") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::box(cf3(0.2f, 0.5f, 0.2f)), cf3(0, 0, 0));
    n.repeat = Repeat::grid_finite(1.4f, cf3(1, 0, 1));
    n.deformers = {scene::Deformer::twist(1.0f)};
    l.sdf->insert(n);

    scene::Tape tape = scene::compile_document(doc);
    clay_test::Lcg rng(1408);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        // each copy is twisted: repetition maps the point first
        float expected = sd_box(ctwist_point(crep_lim_point(p, 1.4f, cf3(1, 0, 1)), 1.0f),
                                cf3(0.2f, 0.5f, 0.2f));
        CHECK(tape.eval(p).d == doctest::Approx(expected).epsilon(1e-5));
    }

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
    scene::Tape reloaded = scene::compile_document(*back);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(tape.eval(p).d == reloaded.eval(p).d);
    }
}
