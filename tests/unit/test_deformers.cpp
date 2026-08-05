#include <doctest/doctest.h>

#include "clay/kernel/deform.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Deformers reachable from a document (sdf-kernels + scene-model deltas of
// add-tape-deformers).

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using scene::Deformer;

namespace {

scene::Document one_item(scene::Prim prim, const std::vector<Deformer>& deformers,
                         cfloat3 pos = cf3(0, 0, 0)) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(prim, pos);
    n.deformers = deformers;
    l.sdf->insert(n);
    return doc;
}

}  // namespace

TEST_CASE("tape twist equals applying the deformer kernel directly") {
    scene::Document doc = one_item(scene::Prim::box(cf3(0.4f, 1.0f, 0.4f)),
                                   {Deformer::twist(1.3f)});
    scene::Tape tape = scene::compile_document(doc);
    clay_test::Lcg rng(1101);
    for (int i = 0; i < 500; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        float expected = sd_box(ctwist_point(p, 1.3f), cf3(0.4f, 1.0f, 0.4f));
        CHECK(tape.eval(p).d == doctest::Approx(expected).epsilon(1e-5));
    }
}

TEST_CASE("tape bend, taper and displace match their kernels") {
    clay_test::Lcg rng(1102);

    scene::Tape bend = scene::compile_document(
        one_item(scene::Prim::capsule(cf3(-1, 0, 0), cf3(1, 0, 0), 0.2f), {Deformer::bend(0.5f)}));
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        float expected = sd_capsule(cbend_point(p, 0.5f), cf3(-1, 0, 0), cf3(1, 0, 0), 0.2f);
        CHECK(bend.eval(p).d == doctest::Approx(expected).epsilon(1e-5));
    }

    scene::Tape taper = scene::compile_document(one_item(
        scene::Prim::capped_cylinder(0.5f, 1.0f), {Deformer::taper(-1.0f, 1.0f, 1.0f, 0.4f)}));
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        // the tape warps the point only; safety comes from the tracked
        // Lipschitz factor, not from scaling the distance (see tape.h)
        cfloat3 q = ctaper_point(p, -1.0f, 1.0f, 1.0f, 0.4f, 0);
        float expected = sd_capped_cylinder(q, 0.5f, 1.0f);
        CHECK(taper.eval(p).d == doctest::Approx(expected).epsilon(1e-5));
    }

    scene::Tape disp = scene::compile_document(
        one_item(scene::Prim::sphere(1.0f), {Deformer::displace(0.06f, 6.0f)}));
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        float g = 0.06f * csin(6.0f * p.x) * csin(6.0f * p.y) * csin(6.0f * p.z);
        CHECK(disp.eval(p).d == doctest::Approx(sd_sphere(p, 1.0f) + g).epsilon(1e-5));
    }
}

TEST_CASE("deformer chains apply in authoring order") {
    // twist (about Y) and bend (in XY) do not commute. Note twist and taper
    // DO commute — both act radially as functions of y — so they are the
    // wrong pair for an order test.
    scene::Prim prim = scene::Prim::box(cf3(0.4f, 1.0f, 0.4f));
    Deformer twist = Deformer::twist(1.2f);
    Deformer bend = Deformer::bend(0.8f);

    scene::Tape a = scene::compile_document(one_item(prim, {twist, bend}));
    scene::Tape b = scene::compile_document(one_item(prim, {bend, twist}));

    clay_test::Lcg rng(1103);
    int differing = 0;
    for (int i = 0; i < 500; ++i) {
        cfloat3 p = rng.vec3(-1.5f, 1.5f);
        // each matches ITS own order applied by hand
        float ta = sd_box(cbend_point(ctwist_point(p, 1.2f), 0.8f), cf3(0.4f, 1.0f, 0.4f));
        float tb = sd_box(ctwist_point(cbend_point(p, 0.8f), 1.2f), cf3(0.4f, 1.0f, 0.4f));
        CHECK(a.eval(p).d == doctest::Approx(ta).epsilon(1e-5));
        CHECK(b.eval(p).d == doctest::Approx(tb).epsilon(1e-5));
        if (cabs(a.eval(p).d - b.eval(p).d) > 1e-4f) ++differing;
    }
    CHECK(differing > 50);  // order genuinely matters
}

TEST_CASE("deformers lower the tracked safe step scale") {
    scene::Tape plain = scene::compile_document(one_item(scene::Prim::box(cf3(1, 1, 1)), {}));
    CHECK(plain.info.is_exact);
    CHECK(plain.safe_step_scale() == doctest::Approx(1.0f));

    scene::Tape twisted =
        scene::compile_document(one_item(scene::Prim::box(cf3(1, 1, 1)), {Deformer::twist(1.5f)}));
    CHECK_FALSE(twisted.info.is_exact);
    CHECK(twisted.safe_step_scale() < 0.6f);  // 1 / (1 + k*r), r = sqrt(2)

    scene::Tape displaced = scene::compile_document(
        one_item(scene::Prim::sphere(1.0f), {Deformer::displace(0.1f, 8.0f)}));
    CHECK(displaced.safe_step_scale() < 1.0f);
}

TEST_CASE("tracked step scale stays conservative under every deformer") {
    struct Case {
        const char* name;
        scene::Document doc;
    };
    std::vector<Case> cases;
    cases.push_back({"twist", one_item(scene::Prim::box(cf3(0.5f, 1.0f, 0.5f)),
                                       {Deformer::twist(1.1f)})});
    cases.push_back({"bend", one_item(scene::Prim::capped_cylinder(0.3f, 1.2f),
                                      {Deformer::bend(0.6f)})});
    cases.push_back({"taper", one_item(scene::Prim::capped_cylinder(0.6f, 1.0f),
                                       {Deformer::taper(-1.0f, 1.0f, 1.0f, 0.4f)})});
    cases.push_back({"displace", one_item(scene::Prim::sphere(1.0f),
                                          {Deformer::displace(0.05f, 5.0f)})});
    cases.push_back({"chain", one_item(scene::Prim::box(cf3(0.4f, 1.0f, 0.4f)),
                                       {Deformer::twist(0.9f),
                                        Deformer::taper(-1.0f, 1.0f, 1.0f, 0.5f)})});
    for (Case& c : cases) {
        CAPTURE(c.name);
        scene::Tape tape = scene::compile_document(c.doc);
        clay_test::check_conservative_steps([&](cfloat3 p) { return tape.eval(p).d; },
                                            tape.safe_step_scale(), 3.0f, 400, 1104);
    }
}

TEST_CASE("influence bounds stay conservative for deformed items") {
    // Same guarantee the scene-model spec states, now with warps: outside the
    // widened bound (dilated by the band), band-clamped values are identical
    // with and without the item.
    const float band = 0.2f;
    struct Case {
        const char* name;
        Deformer def;
    };
    const Case cases[] = {
        {"twist", Deformer::twist(1.4f)},
        {"bend", Deformer::bend(0.7f)},
        {"taper", Deformer::taper(-1.0f, 1.0f, 1.6f, 0.5f)},
        {"displace", Deformer::displace(0.12f, 4.0f)},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(scene::Prim::sphere(1.2f), cf3(0, 0, 0)));  // base field
        scene::Node deformed = item(scene::Prim::box(cf3(0.35f, 0.9f, 0.35f)),
                                    cf3(2.2f, 0.3f, 0.1f), scene::Op::Add,
                                    scene::Blend{scene::BlendProfile::Quadratic, 0.05f});
        deformed.deformers = {c.def};
        scene::NodeId id = l.sdf->insert(deformed);

        math::Aabb bound = scene::item_influence_bound(*l.sdf->find(id), l).dilated(band);
        scene::Tape full = scene::compile_document(doc);
        l.sdf->remove(id);
        scene::Tape without = scene::compile_document(doc);

        clay_test::Lcg rng(1105);
        int outside = 0;
        for (int i = 0; i < 4000; ++i) {
            cfloat3 p = rng.vec3(-5, 5);
            if (bound.contains(p)) continue;
            ++outside;
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(without.eval(p).d, -band, band));
        }
        CHECK(outside > 500);
    }
}

TEST_CASE("per-brick culled tapes stay band-clamp identical with deformers") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.9f), cf3(0, 0, 0)));
    scene::Node twisted = item(scene::Prim::box(cf3(0.3f, 0.8f, 0.3f)), cf3(1.1f, 0, 0),
                               scene::Op::Add, scene::Blend{scene::BlendProfile::Quadratic, 0.08f});
    twisted.deformers = {Deformer::twist(1.5f)};
    l.sdf->insert(twisted);
    scene::Node tapered = item(scene::Prim::capped_cylinder(0.35f, 0.7f), cf3(-1.2f, 0.2f, 0),
                               scene::Op::Subtract, scene::Blend{scene::BlendProfile::Cubic, 0.05f});
    tapered.deformers = {Deformer::taper(-0.7f, 0.7f, 1.3f, 0.5f)};
    l.sdf->insert(tapered);

    scene::Tape full = scene::compile_document(doc);
    const float band = 0.12f;
    clay_test::Lcg rng(1106);
    for (int b = 0; b < 25; ++b) {
        cfloat3 corner = rng.vec3(-3, 3);
        math::Aabb brick{corner, corner + cf3(0.35f, 0.35f, 0.35f)};
        scene::CullRegion cull{brick.dilated(band)};
        scene::Tape culled = scene::compile_document(doc, &cull);
        for (int i = 0; i < 120; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(culled.eval(p).d, -band, band));
        }
    }
}

TEST_CASE("deformed documents round-trip through serialization") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::box(cf3(0.4f, 1.0f, 0.4f)), cf3(0.1f, 0, 0));
    n.deformers = {Deformer::twist(1.2f), Deformer::taper(-1.0f, 1.0f, 1.0f, 0.6f, 3),
                   Deformer::displace(0.05f, 7.0f)};
    l.sdf->insert(n);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);

    const scene::Node* rn = back->layers[0].sdf->find(back->layers[0].sdf->roots[0]);
    REQUIRE(rn != nullptr);
    REQUIRE(rn->deformers.size() == 3);
    CHECK(rn->deformers[0].k == doctest::Approx(1.2f));
    CHECK(rn->deformers[1].ease == 3);

    scene::Tape a = scene::compile_document(doc);
    scene::Tape b = scene::compile_document(*back);
    clay_test::Lcg rng(1107);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(a.eval(p).d == b.eval(p).d);  // bit-identical
    }
}

TEST_CASE("undo restores a deformer chain") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::sphere(1.0f), cf3(0, 0, 0));
    n.deformers = {Deformer::twist(0.8f)};
    scene::NodeId id = l.sdf->insert(n);
    std::vector<std::uint8_t> before = scene::serialize_document(doc);

    scene::UndoStack undo;
    REQUIRE(undo.perform(doc, scene::RemoveNodeCmd{l.id, id}));
    CHECK(scene::serialize_document(doc) != before);
    REQUIRE(undo.undo(doc));
    CHECK(scene::serialize_document(doc) == before);  // deformers came back
}


// --- transition morphs (add-transition-morphs) ------------------------------

namespace {

// sphere at the origin, then a box morphed in by a transition
scene::Document morph_doc(scene::Op op, const scene::Transition& t) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node base = item(scene::Prim::sphere(0.8f), cf3(0, 0, 0));
    base.color = cf3(1, 0, 0);
    l.sdf->insert(base);
    scene::Node morph = item(scene::Prim::box(cf3(0.6f, 0.6f, 0.6f)), cf3(0, 0, 0), op);
    morph.color = cf3(0, 0, 1);
    morph.transition = t;
    l.sdf->insert(morph);
    return doc;
}

}  // namespace

TEST_CASE("transition weight endpoints select each operand") {
    scene::Transition t;
    t.a = cf3(0, -2, 0);
    t.b = cf3(0, 2, 0);
    scene::Document doc = morph_doc(scene::Op::TransitionLinear, t);
    scene::Tape tape = scene::compile_document(doc);

    // at the segment start the accumulated (sphere) field wins outright,
    // at the end the item (box) field does
    for (float radius : {0.3f, 1.5f, 3.0f}) {
        cfloat3 at_start = cf3(radius, -2, 0);
        cfloat3 at_end = cf3(radius, 2, 0);
        CHECK(tape.eval(at_start).d ==
              doctest::Approx(sd_sphere(at_start, 0.8f)).epsilon(1e-5));
        CHECK(tape.eval(at_end).d ==
              doctest::Approx(sd_box(at_end, cf3(0.6f, 0.6f, 0.6f))).epsilon(1e-5));
    }
    // and the colors follow the same weight
    CHECK(tape.eval(cf3(0.2f, -2, 0)).color.x > 0.99f);
    CHECK(tape.eval(cf3(0.2f, 2, 0)).color.z > 0.99f);
}

TEST_CASE("transitions match the kernel weight (linear and radial)") {
    scene::Transition lin;
    lin.a = cf3(-1.5f, 0, 0);
    lin.b = cf3(1.5f, 0, 0);
    lin.ease = kernel::ease_smoothstep;
    scene::Tape linear = scene::compile_document(morph_doc(scene::Op::TransitionLinear, lin));

    scene::Transition rad;
    rad.r0 = 0.5f;
    rad.r1 = 2.0f;
    rad.ease = kernel::ease_in_out_cubic;
    scene::Tape radial = scene::compile_document(morph_doc(scene::Op::TransitionRadial, rad));

    clay_test::Lcg rng(1201);
    for (int i = 0; i < 800; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        float a = sd_sphere(p, 0.8f);
        float b = sd_box(p, cf3(0.6f, 0.6f, 0.6f));

        float wl = ctransition_linear_weight(p, lin.a, lin.b, lin.ease);
        CHECK(linear.eval(p).d == doctest::Approx(cmix(a, b, wl)).epsilon(1e-5));

        float wr = ctransition_radial_weight(p, rad.r0, rad.r1, rad.ease);
        CHECK(radial.eval(p).d == doctest::Approx(cmix(a, b, wr)).epsilon(1e-5));
    }
}

TEST_CASE("transitions are non-exact and step conservatively") {
    scene::Transition t;
    t.a = cf3(0, -1.5f, 0);
    t.b = cf3(0, 1.5f, 0);
    scene::Tape tape = scene::compile_document(morph_doc(scene::Op::TransitionLinear, t));
    CHECK_FALSE(tape.info.is_exact);
    CHECK(tape.safe_step_scale() < 1.0f);
    clay_test::check_conservative_steps([&](cfloat3 p) { return tape.eval(p).d; },
                                        tape.safe_step_scale(), 3.0f, 500, 1202);
}

TEST_CASE("transition items report infinite influence and are never culled") {
    scene::Transition t;
    t.a = cf3(0, -1.5f, 0);
    t.b = cf3(0, 1.5f, 0);
    scene::Document doc = morph_doc(scene::Op::TransitionLinear, t);
    scene::Layer& l = doc.layers[0];
    const scene::Node* morph = l.sdf->find(l.sdf->roots[1]);
    REQUIRE(morph != nullptr);
    CHECK(scene::item_influence_bound(*morph, l).is_infinite());
    // the geometric bound stays finite so meshing and raycast clipping work
    CHECK_FALSE(scene::item_geometry_bound(*morph, l).is_infinite());

    scene::Tape full = scene::compile_document(doc);
    CHECK_FALSE(full.bounds.is_infinite());

    // even a brick far from both operands keeps the morph
    const float band = 0.1f;
    clay_test::Lcg rng(1203);
    for (int b = 0; b < 20; ++b) {
        cfloat3 corner = rng.vec3(-6, 6);
        math::Aabb brick{corner, corner + cf3(0.3f, 0.3f, 0.3f)};
        scene::CullRegion cull{brick.dilated(band)};
        scene::Tape culled = scene::compile_document(doc, &cull);
        for (int i = 0; i < 60; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(culled.eval(p).d, -band, band));
        }
    }
}

TEST_CASE("rigid blends are still culled alongside a transition") {
    scene::Transition t;
    t.a = cf3(0, -1.5f, 0);
    t.b = cf3(0, 1.5f, 0);
    scene::Document doc = morph_doc(scene::Op::TransitionLinear, t);
    // a local item far away must still be dropped for a distant brick
    doc.layers[0].sdf->insert(item(scene::Prim::sphere(0.2f), cf3(9, 9, 9), scene::Op::Add,
                                   scene::Blend{scene::BlendProfile::Quadratic, 0.05f}));
    scene::Tape full = scene::compile_document(doc);
    scene::CullRegion cull{math::Aabb{cf3(-0.2f, -0.2f, -0.2f), cf3(0.2f, 0.2f, 0.2f)}};
    scene::Tape culled = scene::compile_document(doc, &cull);
    CHECK(culled.instrs.size() < full.instrs.size());  // the distant sphere went
}

TEST_CASE("transition parameters round-trip through serialization") {
    scene::Transition t;
    t.a = cf3(0.5f, -1.0f, 0.25f);
    t.b = cf3(-0.5f, 2.0f, 0.75f);
    t.r0 = 0.3f;
    t.r1 = 1.7f;
    t.ease = kernel::ease_out_back;
    scene::Document doc = morph_doc(scene::Op::TransitionRadial, t);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);

    scene::Tape a = scene::compile_document(doc);
    scene::Tape b = scene::compile_document(*back);
    clay_test::Lcg rng(1204);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(a.eval(p).d == b.eval(p).d);
    }
}

TEST_CASE("tape transition matches the reference tree evaluator") {
    scene::Transition t;
    t.a = cf3(-1, -1, 0);
    t.b = cf3(1, 2, 0.5f);
    t.ease = kernel::ease_in_out_quad;
    scene::Document doc = morph_doc(scene::Op::TransitionLinear, t);
    scene::Tape tape = scene::compile_document(doc);
    clay_test::Lcg rng(1205);
    for (int i = 0; i < 500; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CTapeValue tv = tape.eval(p);
        CTapeValue rv = clay_test::ref_eval_document(doc, p);
        CHECK(tv.d == doctest::Approx(rv.d).epsilon(1e-5));
        CHECK(clength(tv.color - rv.color) < 1e-4f);
    }
}

// --- wrap_around (add-wrap-around-opcode) ------------------------------------

TEST_CASE("wrap_around bends the interval around the Z axis") {
    const float x0 = -3.14159265f, x1 = 3.14159265f;   // per = 2pi -> r = 1
    const float r = (x1 - x0) * 0.15915494f;

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::box(cf3(3.14f, 0.2f, 0.5f)), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::wrap_around(x0, x1));
    scene::NodeId id = l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(4242);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-3, 3), rng.range(-1, 1));
            cfloat3 flat = cwrap_around_point(p, x0, x1);
            float want = sd_box(flat, cf3(3.14f, 0.2f, 0.5f));
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("the surface sits on the cylinder of radius r") {
        // a point on the cylinder is inside the slab's half-thickness
        CHECK(tape.eval(cf3(r, 0, 0)).d < 0.0f);
        CHECK(tape.eval(cf3(0, r, 0)).d < 0.0f);
        // the axis is about r - halfthickness away from the inner surface
        CHECK(tape.eval(cf3(0, 0, 0)).d == doctest::Approx(r - 0.2f).epsilon(1e-3));
    }

    SUBCASE("the bound contains the wrapped geometry") {
        const scene::Node* node = l.sdf->find(id);
        REQUIRE(node != nullptr);
        math::Aabb bound = scene::item_geometry_bound(*node, l);

        clay_test::Lcg rng(99);
        int outside = 0;
        for (int i = 0; i < 20000; ++i) {
            cfloat3 p = cf3(rng.range(-4, 4), rng.range(-4, 4), rng.range(-2, 2));
            if (tape.eval(p).d > 0.0f) continue;          // only surface/interior
            if (p.x < bound.min.x || p.x > bound.max.x || p.y < bound.min.y ||
                p.y > bound.max.y || p.z < bound.min.z || p.z > bound.max.z)
                ++outside;
        }
        CHECK(outside == 0);
    }

    SUBCASE("wrapping downgrades exactness and the step scale") {
        CHECK(tape.info.is_exact == false);
        CHECK(tape.safe_step_scale() < 1.0f);
        // cfi_wrap_around: L = 1 + thickness / r, thickness = 0.2, r = 1
        CHECK(tape.safe_step_scale() == doctest::Approx(1.0f / 1.2f).epsilon(1e-3));
    }
}

TEST_CASE("wrap_around composes in a deformer chain, in order") {
    auto build = [](bool wrap_first) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n = clay_test::item(scene::Prim::box(cf3(3.0f, 0.2f, 0.4f)), cf3(0, 0, 0));
        if (wrap_first) {
            n.deformers.push_back(scene::Deformer::wrap_around(-3.0f, 3.0f));
            n.deformers.push_back(scene::Deformer::twist(0.8f));
        } else {
            n.deformers.push_back(scene::Deformer::twist(0.8f));
            n.deformers.push_back(scene::Deformer::wrap_around(-3.0f, 3.0f));
        }
        l.sdf->insert(n);
        return scene::compile_document(doc);
    };
    scene::Tape a = build(true), b = build(false);

    clay_test::Lcg rng(7);
    int differing = 0;
    for (int i = 0; i < 2048; ++i) {
        cfloat3 p = cf3(rng.range(-2, 2), rng.range(-2, 2), rng.range(-1, 1));
        if (kernel::cabs(a.eval(p).d - b.eval(p).d) > 1e-3f) ++differing;
    }
    CHECK(differing > 0);  // the chain does not commute
}

// --- elongate (add-elongate-opcode) ------------------------------------------

TEST_CASE("elongate inserts flat sections without distorting the ends") {
    const cfloat3 h = cf3(1.0f, 0.0f, 0.0f);

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::elongate(h));
    scene::NodeId id = l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(31337);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-2, 2), rng.range(-2, 2));
            float correction = 0.0f;
            cfloat3 q = celongate_point(p, h, &correction);
            float want = sd_sphere(q, 0.5f) + correction;
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("a stretched sphere is a capsule") {
        // flat along the inserted section, and the cap is undistorted
        CHECK(tape.eval(cf3(0, 0, 0)).d == doctest::Approx(-0.5f).epsilon(1e-4));
        CHECK(tape.eval(cf3(0.9f, 0, 0)).d == doctest::Approx(-0.5f).epsilon(1e-4));
        CHECK(tape.eval(cf3(1.5f, 0, 0)).d == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(tape.eval(cf3(0, 0.5f, 0)).d == doctest::Approx(0.0f).epsilon(1e-3));
        // it really is a capsule: the cap centre is at x = h
        CHECK(tape.eval(cf3(2.0f, 0, 0)).d == doctest::Approx(0.5f).epsilon(1e-3));
    }

    SUBCASE("an origin-symmetric primitive stays exact") {
        CHECK(tape.info.is_exact);
        CHECK(tape.safe_step_scale() == doctest::Approx(1.0f));
    }

    SUBCASE("the bound contains the stretched geometry") {
        const scene::Node* node = l.sdf->find(id);
        REQUIRE(node != nullptr);
        math::Aabb bound = scene::item_geometry_bound(*node, l);

        clay_test::Lcg rng(555);
        int outside = 0;
        for (int i = 0; i < 20000; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-2, 2), rng.range(-2, 2));
            if (tape.eval(p).d > 0.0f) continue;
            if (p.x < bound.min.x || p.x > bound.max.x || p.y < bound.min.y ||
                p.y > bound.max.y || p.z < bound.min.z || p.z > bound.max.z)
                ++outside;
        }
        CHECK(outside == 0);
    }
}

TEST_CASE("elongating an asymmetric primitive drops exactness but not the step scale") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::capped_cone(0.8f, 0.6f, 0.1f), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::elongate(cf3(0.5f, 0, 0)));
    l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    // The correction is derived about the origin, so it is only a bound here.
    CHECK(tape.info.is_exact == false);
    // But the map is non-expansive, so tracing is not slowed.
    CHECK(tape.safe_step_scale() == doctest::Approx(1.0f));
}

// --- ramped bends (add-bend-opcodes) -----------------------------------------

TEST_CASE("bend_linear displaces only across its span") {
    const cfloat3 a = cf3(0, -1, 0), b = cf3(0, 1, 0), v = cf3(0.8f, 0, 0.2f);
    const int ease = 3;

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::box(cf3(0.3f, 1.0f, 0.3f)), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::bend_linear(a, b, v, static_cast<std::uint8_t>(ease)));
    scene::NodeId id = l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(818);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2));
            float want = sd_box(cbend_linear_point(p, a, b, v, ease), cf3(0.3f, 1.0f, 0.3f));
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("the ramp runs from nothing to the full vector") {
        // below the segment start: undisplaced, so the box face is at x = 0.3
        CHECK(tape.eval(cf3(0.3f, -1.0f, 0)).d == doctest::Approx(0.0f).epsilon(1e-3));
        // past the end: displaced by the whole vector
        CHECK(tape.eval(cf3(0.3f + v.x, 1.0f, v.z)).d == doctest::Approx(0.0f).epsilon(1e-3));
    }

    SUBCASE("the easing curve reaches the field") {
        scene::Document other;
        scene::Layer& ol = other.add_sdf_layer("l");
        scene::Node m = clay_test::item(scene::Prim::box(cf3(0.3f, 1.0f, 0.3f)), cf3(0, 0, 0));
        m.deformers.push_back(scene::Deformer::bend_linear(a, b, v, 0));  // linear
        ol.sdf->insert(m);
        scene::Tape linear = scene::compile_document(other);

        clay_test::Lcg rng(3);
        int differing = 0;
        for (int i = 0; i < 2048; ++i) {
            cfloat3 p = cf3(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
            if (kernel::cabs(tape.eval(p).d - linear.eval(p).d) > 1e-3f) ++differing;
        }
        CHECK(differing > 0);
    }

    SUBCASE("the bound contains the displaced geometry") {
        const scene::Node* node = l.sdf->find(id);
        REQUIRE(node != nullptr);
        math::Aabb bound = scene::item_geometry_bound(*node, l);
        clay_test::Lcg rng(64);
        int outside = 0;
        for (int i = 0; i < 20000; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-3, 3), rng.range(-3, 3));
            if (tape.eval(p).d > 0.0f) continue;
            if (p.x < bound.min.x || p.x > bound.max.x || p.y < bound.min.y ||
                p.y > bound.max.y || p.z < bound.min.z || p.z > bound.max.z)
                ++outside;
        }
        CHECK(outside == 0);
    }

    SUBCASE("the ramp slope reaches the step scale") {
        // cfi_bend_linear: L = 1 + |v| / |b - a|
        float expect = 1.0f / (1.0f + kernel::clength(v) / 2.0f);
        CHECK(tape.safe_step_scale() == doctest::Approx(expect).epsilon(1e-3));
    }
}

TEST_CASE("bend_radial displaces only across its band") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::capped_cylinder(1.2f, 0.15f), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::bend_radial(0.2f, 1.2f, 0.6f, 5));
    l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(2024);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2));
            float want = sd_capped_cylinder(cbend_radial_point(p, 0.2f, 1.2f, 0.6f, 5), 1.2f,
                                            0.15f);
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("inside r0 the disc is where it was, past r1 it is lifted") {
        CHECK(tape.eval(cf3(0, 0, 0)).d < 0.0f);            // centre unmoved
        CHECK(tape.eval(cf3(1.1f, 0.6f, 0)).d < 0.0f);      // rim lifted by dz
    }
}

TEST_CASE("a document saved before the wide deformers still loads") {
    // The type is written before its parameters, so a deformer that carries no
    // extension floats decodes exactly as it always did.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::box(cf3(0.4f, 0.9f, 0.4f)), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::twist(1.2f));
    n.deformers.push_back(scene::Deformer::taper(-0.9f, 0.9f, 1.0f, 0.4f, 2));
    l.sdf->insert(n);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
}

// --- elongate_axis (add-elongate-axis-opcode) --------------------------------

TEST_CASE("elongate_axis stretches any primitive, as a bound") {
    const cfloat3 h = cf3(0.7f, 0.0f, 0.3f);

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::capped_cone(0.6f, 0.5f, 0.1f), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::elongate_axis(h));
    scene::NodeId id = l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(9090);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-2, 2), rng.range(-2, 2));
            float want = sd_capped_cone(celongate_axis_point(p, h), 0.6f, 0.5f, 0.1f);
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("the interior plateau is flat across the stretch") {
        float centre = tape.eval(cf3(0, 0, 0)).d;
        CHECK(tape.eval(cf3(0.7f, 0, 0)).d == doctest::Approx(centre).epsilon(1e-4));
        CHECK(tape.eval(cf3(-0.7f, 0, 0)).d == doctest::Approx(centre).epsilon(1e-4));
    }

    SUBCASE("always a bound, but never slower") {
        CHECK(tape.info.is_exact == false);
        CHECK(tape.safe_step_scale() == doctest::Approx(1.0f));
    }

    SUBCASE("even an origin-symmetric primitive is a bound") {
        scene::Document sym;
        scene::Layer& sl = sym.add_sdf_layer("l");
        scene::Node m = clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0));
        m.deformers.push_back(scene::Deformer::elongate_axis(cf3(0.5f, 0, 0)));
        sl.sdf->insert(m);
        // elongate would stay exact here; elongate_axis cannot, because the
        // plateau is not a distance.
        CHECK(scene::compile_document(sym).info.is_exact == false);
    }

    SUBCASE("the bound contains the stretched geometry") {
        const scene::Node* node = l.sdf->find(id);
        REQUIRE(node != nullptr);
        math::Aabb bound = scene::item_geometry_bound(*node, l);
        clay_test::Lcg rng(4321);
        int outside = 0;
        for (int i = 0; i < 20000; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-2, 2), rng.range(-2, 2));
            if (tape.eval(p).d > 0.0f) continue;
            if (p.x < bound.min.x || p.x > bound.max.x || p.y < bound.min.y ||
                p.y > bound.max.y || p.z < bound.min.z || p.z > bound.max.z)
                ++outside;
        }
        CHECK(outside == 0);
    }
}

// --- region deformers: grab and pose (add-region-deformers) ------------------

namespace {

scene::Tape grabbed(cfloat3 centre, float radius, cfloat3 disp, std::uint8_t ease,
                    bool front_only, scene::Document& keep) {
    scene::Layer& l = keep.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::sphere(1.0f), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::grab(centre, radius, disp, ease, front_only));
    l.sdf->insert(n);
    return scene::compile_document(keep);
}

scene::Tape plain_sphere(scene::Document& keep) {
    scene::Layer& l = keep.add_sdf_layer("l");
    l.sdf->insert(clay_test::item(scene::Prim::sphere(1.0f), cf3(0, 0, 0)));
    return scene::compile_document(keep);
}

}  // namespace

TEST_CASE("grab moves a region and leaves the rest exactly alone") {
    const cfloat3 centre = cf3(1.0f, 0, 0), disp = cf3(0.5f, 0, 0);
    const float radius = 0.8f;
    scene::Document doc, plain;
    scene::Tape tape = grabbed(centre, radius, disp, 0, false, doc);
    scene::Tape base = plain_sphere(plain);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(1234);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-3, 3), rng.range(-3, 3));
            float want = sd_sphere(cgrab_point(p, centre, radius, disp, 0.0f, 0), 1.0f);
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("outside the radius the field is bit-identical to the undeformed one") {
        clay_test::Lcg rng(99);
        int leaked = 0;
        for (int i = 0; i < 20000; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-3, 3), rng.range(-3, 3));
            if (kernel::clength(p - centre) <= radius) continue;
            if (tape.eval(p).d != base.eval(p).d) ++leaked;
        }
        CHECK(leaked == 0);  // finite support is what keeps culling valid
    }

    SUBCASE("the surface moved toward the pull") {
        // the old tip is now interior, and material exists beyond it
        CHECK(base.eval(cf3(1.0f, 0, 0)).d == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(tape.eval(cf3(1.0f, 0, 0)).d < -1e-3f);
        CHECK(tape.eval(cf3(1.2f, 0, 0)).d < base.eval(cf3(1.2f, 0, 0)).d);
    }

    SUBCASE("the falloff curve shapes the pull") {
        scene::Document eased;
        scene::Tape other = grabbed(centre, radius, disp, 5, false, eased);
        clay_test::Lcg rng(7);
        int differing = 0, outside_differing = 0;
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2));
            bool inside = kernel::clength(p - centre) <= radius;
            if (kernel::cabs(tape.eval(p).d - other.eval(p).d) > 1e-4f) {
                if (inside) ++differing;
                else ++outside_differing;
            }
        }
        CHECK(differing > 0);          // the curve changes the result within the region
        CHECK(outside_differing == 0); // and nowhere else
    }

    SUBCASE("it is a bound, and tracing slows by the ramp slope") {
        CHECK(tape.info.is_exact == false);
        CHECK(tape.safe_step_scale() < 1.0f);
    }
}

TEST_CASE("grab's front-facing option leaves the far side where it was") {
    const cfloat3 centre = cf3(0, 0, 0), disp = cf3(0.6f, 0, 0);
    scene::Document open_doc, gated_doc, plain;
    scene::Tape open_grab = grabbed(centre, 2.0f, disp, 0, false, open_doc);
    scene::Tape gated = grabbed(centre, 2.0f, disp, 0, true, gated_doc);
    scene::Tape base = plain_sphere(plain);

    // A point well behind the centre relative to the pull direction.
    cfloat3 behind = cf3(-1.0f, 0, 0);
    CHECK(open_grab.eval(behind).d != doctest::Approx(base.eval(behind).d).epsilon(1e-4));
    CHECK(gated.eval(behind).d == doctest::Approx(base.eval(behind).d).epsilon(1e-3));
    // ...while the near side still moves under both.
    cfloat3 front = cf3(1.0f, 0, 0);
    CHECK(gated.eval(front).d != doctest::Approx(base.eval(front).d).epsilon(1e-4));
}

TEST_CASE("grab's bound contains the pulled geometry") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::sphere(1.0f), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::grab(cf3(1.0f, 0, 0), 0.8f, cf3(0.5f, 0.3f, 0), 0));
    scene::NodeId id = l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    const scene::Node* node = l.sdf->find(id);
    REQUIRE(node != nullptr);
    math::Aabb bound = scene::item_geometry_bound(*node, l);

    clay_test::Lcg rng(31);
    int outside = 0;
    for (int i = 0; i < 20000; ++i) {
        cfloat3 p = cf3(rng.range(-3, 3), rng.range(-3, 3), rng.range(-3, 3));
        if (tape.eval(p).d > 0.0f) continue;
        if (p.x < bound.min.x || p.x > bound.max.x || p.y < bound.min.y ||
            p.y > bound.max.y || p.z < bound.min.z || p.z > bound.max.z)
            ++outside;
    }
    CHECK(outside == 0);
}

TEST_CASE("pose rotates a region about its centre") {
    const cfloat3 centre = cf3(0, 0.8f, 0), axis = cf3(0, 0, 1);
    const float radius = 1.0f, angle = 0.7f;

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::capped_cylinder(0.3f, 1.0f), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::pose(centre, radius, axis, angle, 0));
    l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(555);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-3, 3), rng.range(-3, 3));
            float want = sd_capped_cylinder(cpose_point(p, centre, radius, axis, angle, 0),
                                            0.3f, 1.0f);
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("the pivot is unmoved and the far end is untouched") {
        // exactly at the centre the rotation is by the full angle about a zero
        // radius, so the point maps to itself
        CHECK(cpose_point(centre, centre, radius, axis, angle, 0).x ==
              doctest::Approx(centre.x).epsilon(1e-5));
        // beyond the radius the map is the identity
        cfloat3 far = cf3(0, -2.5f, 0);
        CHECK(cpose_point(far, centre, radius, axis, angle, 0).y ==
              doctest::Approx(far.y).epsilon(1e-5));
    }

    SUBCASE("it is a bound and tracing slows") {
        CHECK(tape.info.is_exact == false);
        CHECK(tape.safe_step_scale() < 1.0f);
    }
}
