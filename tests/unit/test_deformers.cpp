#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "clay/kernel/deform.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/curve.h"
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


// A guide as HARD control points, so it compiles to itself and the test knows
// exactly which polyline the field was built from — no tessellation standing
// between the assertion and the geometry.
std::vector<scene::StrokePoint> polyline(const std::vector<cfloat3>& pts) {
    std::vector<scene::StrokePoint> out;
    for (cfloat3 p : pts) {
        scene::StrokePoint sp;
        sp.pos = p;
        sp.type = scene::StrokePointType::Hard;
        out.push_back(sp);
    }
    return out;
}

// A quarter circle in the XY plane, from (r,0,0) round to (0,r,0), sampled
// finely enough that the polyline is a fair stand-in for the arc.
std::vector<scene::StrokePoint> quarter_arc(float r, int segments) {
    std::vector<cfloat3> pts;
    for (int i = 0; i <= segments; ++i) {
        const float a = 1.5707963f * static_cast<float>(i) / static_cast<float>(segments);
        pts.push_back(cf3(r * std::cos(a), r * std::sin(a), 0.0f));
    }
    return polyline(pts);
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
    // The ranged pair, EASED: an eased ramp is steeper somewhere in the middle
    // than its average rate — smoothstep peaks at 1.5x — so this is the case
    // that catches a bound derived from the average.
    cases.push_back({"twist_range", one_item(scene::Prim::box(cf3(0.5f, 1.0f, 0.5f)),
                                             {Deformer::twist_range(1.6f, -0.5f, 0.5f, 3)})});
    cases.push_back({"bend_range", one_item(scene::Prim::capped_cylinder(0.3f, 1.2f),
                                            {Deformer::bend_range(1.0f, -0.4f, 0.4f, 3)})});
    // A guide that TURNS. The bound has to cover both the curvature
    // compression on the inside of the bend and the rescale from laying the
    // item's span onto the guide's arc length; a case with a straight guide
    // would pass with neither term wired in.
    cases.push_back({"bend_curve", one_item(scene::Prim::box(cf3(0.9f, 0.2f, 0.2f)),
                                            {Deformer::bend_curve(quarter_arc(1.1f, 32),
                                                                  -0.9f, 0.9f)})});
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

// --- pose along a line (add-pose-line-regions) -------------------------------

namespace {

scene::Tape line_posed(float angle, std::uint8_t ease, scene::Document& keep,
                       scene::NodeId* out_id = nullptr) {
    scene::Layer& l = keep.add_sdf_layer("l");
    scene::Node n = clay_test::item(
        scene::Prim::capsule(cf3(0, -1, 0), cf3(0, 1, 0), 0.25f), cf3(0, 0, 0));
    n.deformers.push_back(
        scene::Deformer::pose_line(cf3(0, -1, 0), cf3(0, 1, 0), cf3(0, 0, 1), angle, ease));
    scene::NodeId id = l.sdf->insert(n);
    if (out_id) *out_id = id;
    return scene::compile_document(keep);
}

// Mean x of the solid samples — how far the form has swung.
float mean_solid_x(const scene::Tape& tape) {
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < 60; ++i)
        for (int j = 0; j < 60; ++j)
            for (int k = 0; k < 20; ++k) {
                cfloat3 p = cf3(-3.0f + 6.0f * i / 59.0f, -2.0f + 4.0f * j / 59.0f,
                                -0.5f + 1.0f * k / 19.0f);
                if (tape.eval(p).d < 0.0f) {
                    sum += p.x;
                    ++n;
                }
            }
    return n ? static_cast<float>(sum / n) : 0.0f;
}

}  // namespace

TEST_CASE("pose_line ramps a rotation along its segment") {
    scene::Document doc;
    scene::Tape tape = line_posed(0.8f, 0, doc);

    SUBCASE("the tape agrees with the kernel applied by hand") {
        clay_test::Lcg rng(606);
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-3, 3), rng.range(-3, 3), rng.range(-2, 2));
            cfloat3 q = cpose_line_point(p, cf3(0, -1, 0), cf3(0, 1, 0), cf3(0, 0, 1), 0.8f, 0);
            float want = sd_capsule(q, cf3(0, -1, 0), cf3(0, 1, 0), 0.25f);
            CHECK(tape.eval(p).d == doctest::Approx(want).epsilon(1e-4));
        }
    }

    SUBCASE("the anchor is a fixed point") {
        scene::Document plain;
        scene::Layer& pl = plain.add_sdf_layer("l");
        pl.sdf->insert(clay_test::item(
            scene::Prim::capsule(cf3(0, -1, 0), cf3(0, 1, 0), 0.25f), cf3(0, 0, 0)));
        scene::Tape base = scene::compile_document(plain);
        CHECK(tape.eval(cf3(0, -1, 0)).d == doctest::Approx(base.eval(cf3(0, -1, 0)).d)
                                                .epsilon(1e-4));
    }

    SUBCASE("the form bends further as the angle grows") {
        scene::Document small_doc, large_doc;
        float none = mean_solid_x(line_posed(0.0f, 0, small_doc));
        float bent = mean_solid_x(line_posed(1.0f, 0, large_doc));
        CHECK(none == doctest::Approx(0.0f).epsilon(0.05));  // straight to begin with
        CHECK(bent < -0.05f);  // swung in the direction of rotation
    }

    SUBCASE("the weight follows the projection, not the distance") {
        // Two points equidistant from the anchor but at different projections
        // onto the segment must receive different weights. A radial region
        // could not tell them apart, which is the whole reason for this mode.
        const cfloat3 a = cf3(0, -1, 0), b = cf3(0, 1, 0);
        cfloat3 along = cf3(0, 0, 0);            // projects half way up
        cfloat3 sideways = cf3(1, -1, 0);        // same distance, projects at 0
        CHECK(kernel::clength(along - a) == doctest::Approx(kernel::clength(sideways - a))
                                                .epsilon(1e-4));
        cfloat3 qa = cpose_line_point(along, a, b, cf3(0, 0, 1), 0.8f, 0);
        cfloat3 qs = cpose_line_point(sideways, a, b, cf3(0, 0, 1), 0.8f, 0);
        CHECK(kernel::clength(qa - along) > 1e-3f);   // moved
        CHECK(kernel::clength(qs - sideways) < 1e-5f);  // did not
    }

    SUBCASE("the easing curve shapes the taper") {
        scene::Document eased;
        scene::Tape other = line_posed(0.8f, 5, eased);
        clay_test::Lcg rng(11);
        int differing = 0;
        for (int i = 0; i < 4096; ++i) {
            cfloat3 p = cf3(rng.range(-2, 2), rng.range(-2, 2), rng.range(-1, 1));
            if (kernel::cabs(tape.eval(p).d - other.eval(p).d) > 1e-3f) ++differing;
        }
        CHECK(differing > 0);
    }

    SUBCASE("it is a bound and tracing slows") {
        CHECK(tape.info.is_exact == false);
        CHECK(tape.safe_step_scale() < 1.0f);
    }
}

TEST_CASE("pose_line's bound contains the swept geometry, including a large angle") {
    for (float angle : {0.5f, 1.5f, 3.0f}) {
        scene::Document doc;
        scene::NodeId id = 0;
        scene::Tape tape = line_posed(angle, 0, doc, &id);
        const scene::Layer& l = doc.layers.front();
        const scene::Node* node = l.sdf->find(id);
        REQUIRE(node != nullptr);
        math::Aabb bound = scene::item_geometry_bound(*node, l);

        clay_test::Lcg rng(77);
        int outside = 0;
        for (int i = 0; i < 20000; ++i) {
            cfloat3 p = cf3(rng.range(-4, 4), rng.range(-4, 4), rng.range(-2, 2));
            if (tape.eval(p).d > 0.0f) continue;
            if (p.x < bound.min.x || p.x > bound.max.x || p.y < bound.min.y ||
                p.y > bound.max.y || p.z < bound.min.z || p.z > bound.max.z)
                ++outside;
        }
        CAPTURE(angle);
        CHECK(outside == 0);
    }
}

TEST_CASE("widening the deformer record left old documents readable") {
    // ext_count dispatches on the deformer type, so a document using only the
    // narrow deformers must still round-trip to identical bytes.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::box(cf3(0.4f, 0.9f, 0.4f)), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::twist(1.1f));
    n.deformers.push_back(scene::Deformer::bend_radial(0.2f, 0.9f, 0.3f, 4));
    l.sdf->insert(n);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
}

TEST_CASE("a ranged twist over its whole span IS the unranged twist") {
    // The property that makes this a RANGE on an existing deformation rather
    // than a second one to keep in step. With a linear ease and a span that
    // covers the content, every point inside the span must warp identically.
    const float k = 1.3f;
    for (float y = -1.0f; y <= 1.0f; y += 0.1f)
        for (float x = -0.7f; x <= 0.7f; x += 0.35f)
            for (float z = -0.7f; z <= 0.7f; z += 0.35f) {
                const cfloat3 p = cf3(x, y, z);
                const cfloat3 plain = kernel::ctwist_point(p, k);
                // range [0, 1] with a linear ease: angle = k*(1-0)*clamp(y,0,1)
                const cfloat3 ranged = kernel::ctwist_range_point(p, k, 0.0f, 1.0f, 0);
                if (y >= 0.0f && y <= 1.0f) {
                    CAPTURE(y);
                    CHECK(ranged.x == doctest::Approx(plain.x).epsilon(1e-5));
                    CHECK(ranged.y == doctest::Approx(plain.y).epsilon(1e-5));
                    CHECK(ranged.z == doctest::Approx(plain.z).epsilon(1e-5));
                }
            }
}

TEST_CASE("a ranged bend over its whole span IS the unranged bend") {
    const float k = 0.9f;
    for (float x = -1.0f; x <= 1.0f; x += 0.1f)
        for (float y = -0.6f; y <= 0.6f; y += 0.3f) {
            const cfloat3 p = cf3(x, y, 0.2f);
            const cfloat3 plain = kernel::cbend_point(p, k);
            const cfloat3 ranged = kernel::cbend_range_point(p, k, 0.0f, 1.0f, 0);
            if (x >= 0.0f && x <= 1.0f) {
                CAPTURE(x);
                CHECK(ranged.x == doctest::Approx(plain.x).epsilon(1e-5));
                CHECK(ranged.y == doctest::Approx(plain.y).epsilon(1e-5));
                CHECK(ranged.z == doctest::Approx(plain.z).epsilon(1e-5));
            }
        }
}

TEST_CASE("outside its span a ranged twist holds, it does not keep winding") {
    // What the range is FOR: a gizmo's box twists what is inside it and moves
    // the rest rigidly. Past y1 the angle is constant, so two points that
    // differ only in height above the span rotate by the same amount.
    const float k = 1.7f, y0 = -0.25f, y1 = 0.25f;
    auto angle_at = [&](float y) {
        const cfloat3 p = kernel::ctwist_range_point(cf3(1.0f, y, 0.0f), k, y0, y1, 0);
        return std::atan2(p.z, p.x);
    };
    const float above_a = angle_at(0.5f), above_b = angle_at(2.0f);
    CHECK(above_a == doctest::Approx(above_b).epsilon(1e-5));
    // ...where the unranged twist keeps winding with height.
    auto plain_angle_at = [&](float y) {
        const cfloat3 p = kernel::ctwist_point(cf3(1.0f, y, 0.0f), k);
        return std::atan2(p.z, p.x);
    };
    CHECK(plain_angle_at(0.5f) != doctest::Approx(plain_angle_at(2.0f)).epsilon(1e-3));

    // Below the span it holds too, at the other end.
    CHECK(angle_at(-0.5f) == doctest::Approx(angle_at(-2.0f)).epsilon(1e-5));
    // And the two ends differ, or the range would be doing nothing.
    CHECK(angle_at(2.0f) != doctest::Approx(angle_at(-2.0f)).epsilon(1e-3));
}

TEST_CASE("a steeper ease on a ranged twist declares a tighter step scale") {
    // The ranged bound is charged the angular rate the ease actually REACHES,
    // not the average rate across the span: an eased ramp is steeper somewhere
    // in the middle — smoothstep peaks at 1.5x linear — and the declaration has
    // to cover the steepest point. So a steeper curve must cost step scale,
    // which is what says the ease is wired into the bound at all rather than
    // being applied only to the warp.
    auto scale_with = [](std::uint8_t ease) {
        scene::Document doc = one_item(scene::Prim::box(cf3(0.5f, 1.0f, 0.5f)),
                                       {Deformer::twist_range(1.6f, -0.5f, 0.5f, ease)});
        return scene::compile_document(doc).safe_step_scale();
    };
    const float linear = scale_with(0), smooth = scale_with(3);
    CAPTURE(linear);
    CAPTURE(smooth);
    CHECK(linear < 1.0f);
    CHECK(smooth < linear);  // steeper somewhere => a tighter bound everywhere
}

// -- bend along a curve (sdf-kernels, bend-along-a-curve) --------------------
//
// The deformer is the INVERSE of a sweep, so what these establish is that it
// undoes the right thing: a straight guide undoes nothing, a point ON the
// guide reads the item's own axis at the matching arc length, and material
// travels to where the guide goes rather than staying where it was.

TEST_CASE("a guide running straight down the axis is the identity") {
    // What makes this a GENERALIZATION of the undeformed item rather than a
    // second deformation to keep in step with it. Asserted through the
    // compiler, so the guide emission and its transported frames are on trial
    // too rather than only the point map.
    const cfloat3 half = cf3(0.9f, 0.3f, 0.2f);
    scene::Document plain = one_item(scene::Prim::box(half), {});
    scene::Document bent = one_item(
        scene::Prim::box(half),
        {Deformer::bend_curve(polyline({cf3(-0.9f, 0, 0), cf3(0, 0, 0), cf3(0.9f, 0, 0)}),
                              -0.9f, 0.9f)});

    scene::Tape a = scene::compile_document(plain);
    scene::Tape b = scene::compile_document(bent);
    clay_test::Lcg rng(1160);
    for (int i = 0; i < 800; ++i) {
        const cfloat3 p = rng.vec3(-1.5f, 1.5f);
        CAPTURE(p.x);
        CAPTURE(p.y);
        CAPTURE(p.z);
        CHECK(b.eval(p).d == doctest::Approx(a.eval(p).d).epsilon(1e-4));
    }
}

TEST_CASE("a point on the guide reads the item's axis at the matching arc length") {
    // The map's core claim, and the one that catches an arc length, a span
    // mapping or a transported frame that is off. ON the guide the
    // perpendicular offset is zero by construction, so the deformed field
    // there must equal the undeformed field at the axis point the arc length
    // corresponds to — whatever the guide is doing in between.
    const float r = 1.0f;
    const int segments = 64;
    const float t0 = -1.0f, t1 = 1.0f;
    std::vector<scene::StrokePoint> guide = quarter_arc(r, segments);

    const cfloat3 half = cf3(1.0f, 0.25f, 0.25f);
    scene::Tape plain = scene::compile_document(one_item(scene::Prim::box(half), {}));
    scene::Tape bent = scene::compile_document(
        one_item(scene::Prim::box(half), {Deformer::bend_curve(guide, t0, t1)}));

    const float total = scene::guide_arc_length(guide);
    REQUIRE(total > 0.0f);
    // Walk the guide's own vertices: each one's arc length is known exactly.
    float s = 0.0f;
    for (std::size_t i = 1; i + 1 < guide.size(); ++i) {
        s += kernel::clength(guide[i].pos - guide[i - 1].pos);
        const float u = s / total;
        const float axis_x = t0 + u * (t1 - t0);
        CAPTURE(u);
        CHECK(bent.eval(guide[i].pos).d ==
              doctest::Approx(plain.eval(cf3(axis_x, 0, 0)).d).epsilon(1e-3));
    }
}

TEST_CASE("material travels to where the guide goes") {
    // A long box on X, bent round a quarter circle: its far end has to end up
    // near the arc's far end, which is a place the undeformed box is nowhere
    // near — and the item's influence bound has to contain it, or culling
    // would throw away the bricks the surface is in.
    const float r = 1.0f;
    std::vector<scene::StrokePoint> guide = quarter_arc(r, 48);
    const float total = scene::guide_arc_length(guide);

    scene::Document doc = one_item(scene::Prim::box(cf3(1.0f, 0.15f, 0.15f)),
                                   {Deformer::bend_curve(guide, -1.0f, 1.0f)});
    scene::Tape tape = scene::compile_document(doc);

    // A point most of the way round the arc. NOT the arc's exact endpoint,
    // which is the image of the box's end FACE and so sits at distance zero —
    // an assertion there would turn on float noise rather than on the material
    // having travelled.
    const float a = 0.85f * 1.5707963f;
    const cfloat3 along_arc = cf3(r * std::cos(a), r * std::sin(a), 0.0f);
    CHECK(tape.eval(along_arc).d < 0.0f);                       // inside the bent box
    // ...and a long way from where the undeformed box reaches.
    scene::Tape plain =
        scene::compile_document(one_item(scene::Prim::box(cf3(1.0f, 0.15f, 0.15f)), {}));
    CHECK(plain.eval(along_arc).d > 0.5f);

    // The influence bound has to contain the travelled material, or culling
    // throws away the very bricks the surface ended up in.
    scene::Node probe = clay_test::item(scene::Prim::box(cf3(1.0f, 0.15f, 0.15f)), cf3(0, 0, 0));
    probe.deformers = {Deformer::bend_curve(guide, -1.0f, 1.0f)};
    const math::Aabb bound = scene::item_geometry_bound(probe, doc.layers[0]);
    CHECK(!bound.empty());
    CHECK(bound.max.y >= r - 1e-3f);
    CAPTURE(total);
}

TEST_CASE("a cross-section wider than the tightest bend degrades rather than lying") {
    // The fold case. A guide is editable after the fact, so this is NOT
    // refused — but the compiled tape has to stop claiming the field is a
    // distance, or the marcher steps straight through a surface.
    std::vector<scene::StrokePoint> tight = quarter_arc(0.20f, 24);
    scene::Tape folded = scene::compile_document(one_item(
        scene::Prim::box(cf3(0.3f, 0.5f, 0.5f)), {Deformer::bend_curve(tight, -0.3f, 0.3f)}));
    // Same item on a gentle guide, for contrast: the tightness is what costs,
    // not the presence of a guide.
    std::vector<scene::StrokePoint> gentle = quarter_arc(4.0f, 24);
    scene::Tape easy = scene::compile_document(one_item(
        scene::Prim::box(cf3(0.3f, 0.5f, 0.5f)), {Deformer::bend_curve(gentle, -0.3f, 0.3f)}));

    CAPTURE(folded.safe_step_scale());
    CAPTURE(easy.safe_step_scale());
    CHECK(folded.safe_step_scale() < 1e-3f);
    CHECK(easy.safe_step_scale() > folded.safe_step_scale());
}

TEST_CASE("a guide with fewer than two points passes the point through") {
    // Not an error: a caller can still fix a degenerate guide by editing it,
    // and dropping the deformer would silently change what the document means.
    const cfloat3 half = cf3(0.5f, 0.5f, 0.5f);
    scene::Tape plain = scene::compile_document(one_item(scene::Prim::box(half), {}));
    scene::Tape one_point = scene::compile_document(one_item(
        scene::Prim::box(half), {Deformer::bend_curve(polyline({cf3(0, 0, 0)}), -1.0f, 1.0f)}));
    scene::Tape no_points = scene::compile_document(
        one_item(scene::Prim::box(half), {Deformer::bend_curve({}, -1.0f, 1.0f)}));

    clay_test::Lcg rng(1161);
    for (int i = 0; i < 200; ++i) {
        const cfloat3 p = rng.vec3(-1.5f, 1.5f);
        CHECK(one_point.eval(p).d == doctest::Approx(plain.eval(p).d).epsilon(1e-5));
        CHECK(no_points.eval(p).d == doctest::Approx(plain.eval(p).d).epsilon(1e-5));
    }
}

TEST_CASE("a bend_curve survives the document round trip, guide and all") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::box(cf3(0.8f, 0.2f, 0.2f)), cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::bend_curve(quarter_arc(0.9f, 5), -0.8f, 0.8f));
    // A second deformer AFTER it, so a guide whose length was mis-encoded
    // would desynchronise the reader rather than merely losing itself.
    n.deformers.push_back(scene::Deformer::twist(0.7f));
    l.sdf->insert(n);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
    // And it still compiles to the same field.
    clay_test::Lcg rng(1162);
    scene::Tape a = scene::compile_document(doc), b = scene::compile_document(*back);
    for (int i = 0; i < 200; ++i) {
        const cfloat3 p = rng.vec3(-2, 2);
        CHECK(b.eval(p).d == doctest::Approx(a.eval(p).d).epsilon(1e-5));
    }
}

// -- a lattice cage on an SDF item (sdf-kernels, lattice-on-sdf-items) -------
//
// The cage's offsets ARE the inverse warp, which is the design decision:
// forward FFD has no closed-form inverse and a deformer must run backwards.
// What that costs is that the warp travels a little LESS than nominal, the
// same character `grab` and `pose` carry — measured below rather than assumed.

namespace {

// Every control point of a cage dragged by the same vector.
Deformer uniform_cage(cfloat3 lo, cfloat3 hi, cfloat3 by, int n = 3) {
    Deformer d = Deformer::lattice(lo, hi, n, n, n);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) d.set_cage_offset(i, j, k, by);
    return d;
}

}  // namespace

TEST_CASE("an untouched cage is the undeformed field") {
    // Offsets rather than positions buys this exactly, with no special case.
    const float r = 0.7f;
    scene::Tape plain = scene::compile_document(one_item(scene::Prim::sphere(r), {}));
    scene::Tape caged = scene::compile_document(one_item(
        scene::Prim::sphere(r), {Deformer::lattice(cf3(-1, -1, -1), cf3(1, 1, 1), 3, 3, 3)}));

    clay_test::Lcg rng(1170);
    for (int i = 0; i < 500; ++i) {
        const cfloat3 p = rng.vec3(-2, 2);
        CAPTURE(p.x);
        CHECK(caged.eval(p).d == doctest::Approx(plain.eval(p).d).epsilon(1e-5));
    }
}

TEST_CASE("a uniformly dragged cage translates the field exactly") {
    // The basis is a partition of unity, so every point picks up the same
    // offset — and since the cage is the INVERSE warp, the field moves by the
    // negative of it. That sign is the whole convention, so it is asserted.
    const float r = 0.6f;
    const cfloat3 by = cf3(0.3f, -0.2f, 0.1f);
    scene::Tape plain = scene::compile_document(one_item(scene::Prim::sphere(r), {}));
    scene::Tape moved = scene::compile_document(
        one_item(scene::Prim::sphere(r), {uniform_cage(cf3(-1, -1, -1), cf3(1, 1, 1), by)}));

    clay_test::Lcg rng(1171);
    for (int i = 0; i < 400; ++i) {
        const cfloat3 p = rng.vec3(-1.5f, 1.5f);
        // The offsets are what was DRAGGED, so the material travels WITH the
        // drag: the field at p is the undeformed field at p - by.
        CHECK(moved.eval(p).d == doctest::Approx(plain.eval(p - by).d).epsilon(1e-5));
    }
}

TEST_CASE("two per axis is exactly trilinear, and the corners interpolate") {
    // Degree is one less than the point count, so there is no separate linear
    // path to keep in step. Checked against a hand-written blend.
    const cfloat3 lo = cf3(-1, -1, -1), hi = cf3(1, 1, 1);
    Deformer d = Deformer::lattice(lo, hi, 2, 2, 2);
    cfloat3 corner[8];
    for (int c = 0; c < 8; ++c) {
        corner[c] = cf3(0.05f * static_cast<float>(c + 1), -0.03f * static_cast<float>(c),
                        0.02f * static_cast<float>(c));
        d.set_cage_offset(c & 1, (c >> 1) & 1, (c >> 2) & 1, corner[c]);
    }
    // The offsets, laid out as the blob will hold them.
    std::vector<float> flat;
    for (const cfloat3& o : d.cage) {
        flat.push_back(o.x);
        flat.push_back(o.y);
        flat.push_back(o.z);
    }

    for (float z = -1.0f; z <= 1.0f; z += 0.5f)
        for (float y = -1.0f; y <= 1.0f; y += 0.5f)
            for (float x = -1.0f; x <= 1.0f; x += 0.5f) {
                const float s = (x + 1) * 0.5f, t = (y + 1) * 0.5f, u = (z + 1) * 0.5f;
                cfloat3 want = cf3(0, 0, 0);
                for (int c = 0; c < 8; ++c) {
                    const float wx = (c & 1) ? s : 1 - s;
                    const float wy = ((c >> 1) & 1) ? t : 1 - t;
                    const float wz = ((c >> 2) & 1) ? u : 1 - u;
                    want = want + corner[c] * (wx * wy * wz);
                }
                // Negated back, since clattice_point subtracts the drag.
                const cfloat3 got =
                    cf3(x, y, z) - clattice_point(flat.data(), 2, 2, 2, lo, hi, cf3(x, y, z));
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(z);
                CHECK(got.x == doctest::Approx(want.x).epsilon(1e-4));
                CHECK(got.y == doctest::Approx(want.y).epsilon(1e-4));
            }

    // A corner control point is interpolated: dragging it moves that corner of
    // the box by the whole amount, and the opposite corner not at all.
    Deformer one = Deformer::lattice(lo, hi, 3, 3, 3);
    one.set_cage_offset(0, 0, 0, cf3(0.5f, 0, 0));
    std::vector<float> f2;
    for (const cfloat3& o : one.cage) {
        f2.push_back(o.x);
        f2.push_back(o.y);
        f2.push_back(o.z);
    }
    CHECK((lo - clattice_point(f2.data(), 3, 3, 3, lo, hi, lo)).x ==
          doctest::Approx(0.5f).epsilon(1e-5));
    CHECK((hi - clattice_point(f2.data(), 3, 3, 3, lo, hi, hi)).x ==
          doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("material outside the cage travels rigidly, it is not drawn onto it") {
    const cfloat3 lo = cf3(-1, -1, -1), hi = cf3(1, 1, 1);
    Deformer d = uniform_cage(lo, hi, cf3(0.0f, 0.4f, 0.0f));
    std::vector<float> flat;
    for (const cfloat3& o : d.cage) {
        flat.push_back(o.x);
        flat.push_back(o.y);
        flat.push_back(o.z);
    }
    const cfloat3 outside = cf3(7.0f, 0.0f, 0.0f);
    const cfloat3 mapped = clattice_point(flat.data(), 3, 3, 3, lo, hi, outside);
    // Carried along by the drag — the SAMPLE moves against it, which is what
    // makes the material move with it — and it keeps its own x.
    CHECK((mapped - outside).y == doctest::Approx(-0.4f).epsilon(1e-5));
    CHECK(mapped.x == doctest::Approx(7.0f).epsilon(1e-5));
}

TEST_CASE("the lattice bound follows how fast the cage varies, not how far") {
    // The reason the bound is taken from the DIFFERENCES. Two cages with the
    // same largest offset: one translates the item rigidly (no gradient at
    // all), the other alternates between neighbours (all gradient).
    const cfloat3 lo = cf3(-1, -1, -1), hi = cf3(1, 1, 1);
    const float amount = 0.4f;

    Deformer rigid = uniform_cage(lo, hi, cf3(amount, 0, 0));
    Deformer alternating = Deformer::lattice(lo, hi, 3, 3, 3);
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                alternating.set_cage_offset(i, j, k, cf3((i % 2) ? amount : -amount, 0, 0));

    const float rigid_scale =
        scene::compile_document(one_item(scene::Prim::sphere(0.7f), {rigid})).safe_step_scale();
    const float alt_scale =
        scene::compile_document(one_item(scene::Prim::sphere(0.7f), {alternating}))
            .safe_step_scale();
    CAPTURE(rigid_scale);
    CAPTURE(alt_scale);
    // A rigid translation costs nothing: the field is just moved.
    CHECK(rigid_scale == doctest::Approx(1.0f).epsilon(1e-5));
    // The alternating cage is the one that actually distorts, and it pays —
    // by exactly the Bernstein derivative bound rather than by some amount.
    // Neighbours differ by 2*amount over an extent of 2, at degree 2:
    //     rate = (n - 1) * |difference| / extent = 2 * 0.8 / 2 = 0.8
    //     scale = 1 / (1 + rate)
    // Asserting the value rather than a threshold is what makes this a test of
    // the formula instead of a test of a number somebody once observed.
    const float rate = 2.0f * (2.0f * amount) / 2.0f;
    CHECK(alt_scale == doctest::Approx(1.0f / (1.0f + rate)).epsilon(1e-4));
    CHECK(alt_scale < rigid_scale);
}

TEST_CASE("a lattice steps conservatively over a non-uniform cage") {
    const cfloat3 lo = cf3(-0.9f, -0.9f, -0.9f), hi = cf3(0.9f, 0.9f, 0.9f);
    Deformer d = Deformer::lattice(lo, hi, 3, 3, 3);
    // A twist-ish cage: the top layer rotated, the middle bulged.
    d.set_cage_offset(0, 2, 0, cf3(0.35f, 0.0f, -0.30f));
    d.set_cage_offset(2, 2, 2, cf3(-0.35f, 0.0f, 0.30f));
    d.set_cage_offset(1, 1, 1, cf3(0.0f, 0.25f, 0.0f));

    scene::Tape tape = scene::compile_document(one_item(scene::Prim::sphere(0.7f), {d}));
    CHECK(tape.safe_step_scale() < 1.0f);
    clay_test::check_conservative_steps([&](cfloat3 p) { return tape.eval(p).d; },
                                        tape.safe_step_scale(), 3.0f, 400, 1172);
}

TEST_CASE("a lattice item is a bound field and its influence contains the warp") {
    const cfloat3 lo = cf3(-0.8f, -0.8f, -0.8f), hi = cf3(0.8f, 0.8f, 0.8f);
    Deformer d = Deformer::lattice(lo, hi, 3, 3, 3);
    d.set_cage_offset(2, 2, 2, cf3(0.6f, 0.0f, 0.0f));

    scene::Document doc = one_item(scene::Prim::sphere(0.5f), {d});
    scene::Node probe = clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0));
    probe.deformers = {d};
    const math::Aabb bound = scene::item_geometry_bound(probe, doc.layers[0]);
    const math::Aabb plain =
        scene::item_geometry_bound(clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)),
                                   doc.layers[0]);
    CHECK(!bound.empty());
    // Grown by the largest offset, which a convex combination cannot exceed.
    CHECK(bound.max.x >= plain.max.x + 0.59f);
    CHECK(scene::deformers_break_exactness(probe));
}

TEST_CASE("a lattice survives the document round trip, cage and all") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::box(cf3(0.5f, 0.5f, 0.5f)), cf3(0, 0, 0));
    Deformer d = Deformer::lattice(cf3(-0.6f, -0.6f, -0.6f), cf3(0.6f, 0.6f, 0.6f), 3, 2, 4);
    d.set_cage_offset(1, 1, 2, cf3(0.2f, -0.1f, 0.05f));
    d.set_cage_offset(2, 0, 3, cf3(-0.15f, 0.2f, 0.0f));
    n.deformers.push_back(d);
    // A second deformer after it, so a mis-encoded cage length desynchronises
    // the reader rather than merely losing itself.
    n.deformers.push_back(Deformer::twist(0.6f));
    l.sdf->insert(n);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);

    clay_test::Lcg rng(1173);
    scene::Tape a = scene::compile_document(doc), b = scene::compile_document(*back);
    for (int i = 0; i < 200; ++i) {
        const cfloat3 p = rng.vec3(-2, 2);
        CHECK(b.eval(p).d == doctest::Approx(a.eval(p).d).epsilon(1e-5));
    }
}

TEST_CASE("the cage's divisions are clamped where the cost lives") {
    // Capped low because this evaluates PER SAMPLE, at nx*ny*nz multiply-adds
    // each time — unlike the mesh lattice, which runs once per vertex.
    Deformer low = Deformer::lattice(cf3(0, 0, 0), cf3(1, 1, 1), 1, 0, -3);
    CHECK(static_cast<int>(low.a) == 2);
    CHECK(static_cast<int>(low.b) == 2);
    CHECK(static_cast<int>(low.c) == 2);
    CHECK(low.cage.size() == 8u);

    Deformer high = Deformer::lattice(cf3(0, 0, 0), cf3(1, 1, 1), 99, 99, 99);
    CHECK(static_cast<int>(high.a) == scene::Deformer::kMaxLatticeDivisions);
    CHECK(high.cage.size() == static_cast<std::size_t>(scene::Deformer::kMaxLatticeDivisions *
                                                       scene::Deformer::kMaxLatticeDivisions *
                                                       scene::Deformer::kMaxLatticeDivisions));

    // An out-of-range control point reads zero and writes nowhere.
    Deformer d = Deformer::lattice(cf3(0, 0, 0), cf3(1, 1, 1), 3, 3, 3);
    d.set_cage_offset(9, 9, 9, cf3(5, 5, 5));
    for (const cfloat3& o : d.cage) CHECK(clength(o) == 0.0f);
    CHECK(clength(d.cage_offset(-1, 0, 0)) == 0.0f);
}

// -- Blob (sdf-kernels, add-blob-brush) --------------------------------------
//
// ZBrush's Blob: an irregular swelling under the brush rather than the smooth
// one `draw` gives. It is `noise` with the finite support `grab` and `magnify`
// have, and the tests below are mostly about that support — a modifier that
// reached past its radius would not be a brush.

TEST_CASE("a blob leaves the field untouched past its radius") {
    // The property that makes it a brush. Checked at the same bar `grab`,
    // `pose` and `magnify` are held to in examples/03: EXACTLY untouched, not
    // nearly.
    const float r = 0.5f;
    scene::Tape plain = scene::compile_document(one_item(scene::Prim::sphere(0.9f), {}));
    scene::Tape blobbed = scene::compile_document(one_item(
        scene::Prim::sphere(0.9f),
        {Deformer::blob(cf3(0.9f, 0, 0), r, 0.15f, 8.0f, 3, 0.5f, 7u, 3)}));

    clay_test::Lcg rng(1190);
    int outside = 0, inside_changed = 0;
    for (int i = 0; i < 2000; ++i) {
        const cfloat3 p = rng.vec3(-2.5f, 2.5f);
        const float d = clength(p - cf3(0.9f, 0, 0));
        if (d > r) {
            CAPTURE(d);
            REQUIRE(blobbed.eval(p).d == doctest::Approx(plain.eval(p).d).epsilon(1e-6));
            ++outside;
        } else if (std::fabs(blobbed.eval(p).d - plain.eval(p).d) > 1e-6f) {
            ++inside_changed;
        }
    }
    CHECK(outside > 0);
    CHECK(inside_changed > 0);  // and it does something inside
}

TEST_CASE("a blob both swells and eats in, within one dab") {
    // What makes it read as blobby rather than as a uniform bulge: the noise
    // is signed, so one region moves the surface both ways.
    const float r = 0.6f;
    scene::Tape plain = scene::compile_document(one_item(scene::Prim::sphere(0.8f), {}));
    scene::Tape blobbed = scene::compile_document(one_item(
        scene::Prim::sphere(0.8f),
        {Deformer::blob(cf3(0.8f, 0, 0), r, 0.2f, 9.0f, 4, 0.5f, 3u, 3)}));

    clay_test::Lcg rng(1191);
    int up = 0, down = 0;
    for (int i = 0; i < 3000; ++i) {
        const cfloat3 p = rng.vec3(-1.5f, 1.5f);
        if (clength(p - cf3(0.8f, 0, 0)) > r * 0.9f) continue;
        const float delta = blobbed.eval(p).d - plain.eval(p).d;
        if (delta > 1e-4f) ++up;
        if (delta < -1e-4f) ++down;
    }
    CHECK(up > 0);
    CHECK(down > 0);
}

TEST_CASE("a blob is a bound field and steps conservatively") {
    // The declared factor charges the noise AND the region's own gradient —
    // a tight radius has a steep weight even where the noise is flat, which
    // is exactly where a blob is used.
    scene::Document doc = one_item(scene::Prim::sphere(0.8f),
                                   {Deformer::blob(cf3(0.8f, 0, 0), 0.4f, 0.18f, 7.0f, 3,
                                                   0.5f, 11u, 3)});
    scene::Tape tape = scene::compile_document(doc);
    CHECK(tape.safe_step_scale() < 1.0f);
    clay_test::check_conservative_steps([&](cfloat3 p) { return tape.eval(p).d; },
                                        tape.safe_step_scale(), 3.0f, 400, 1192);

    // A TIGHTER radius costs more step scale at the same amplitude, which is
    // the region term doing its job — without it the two would tie.
    auto scale_with = [](float radius) {
        return scene::compile_document(
                   one_item(scene::Prim::sphere(0.8f),
                            {Deformer::blob(cf3(0.8f, 0, 0), radius, 0.18f, 7.0f, 3, 0.5f, 11u,
                                            3)}))
            .safe_step_scale();
    };
    CHECK(scale_with(0.2f) < scale_with(0.8f));
}

TEST_CASE("a blob's influence bound grows by its amplitude, not its centre") {
    // The bug this catches: sharing noise's hull case would dilate by `k`,
    // which for a blob is the region's centre X rather than the amplitude —
    // a wrong bound shows up as culled-away geometry, not as a crash.
    const float amplitude = 0.25f;
    scene::Document doc = one_item(scene::Prim::sphere(0.5f), {});
    scene::Node probe = clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0));
    probe.deformers = {Deformer::blob(cf3(3.0f, 0, 0), 0.4f, amplitude, 6.0f, 3, 0.5f, 0u, 3)};
    const math::Aabb bound = scene::item_geometry_bound(probe, doc.layers[0]);
    const math::Aabb plain = scene::item_geometry_bound(
        clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)), doc.layers[0]);

    // Grown by the amplitude (0.25), NOT by the centre's x (3.0).
    CHECK(bound.max.x == doctest::Approx(plain.max.x + amplitude).epsilon(1e-4));
    CHECK(bound.max.x < plain.max.x + 1.0f);
    CHECK(scene::deformers_break_exactness(probe) == false);  // noise-like, not elongate-like
}

TEST_CASE("a blob survives the document round trip") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::sphere(0.7f), cf3(0, 0, 0));
    n.deformers.push_back(Deformer::blob(cf3(0.7f, 0.1f, 0), 0.45f, 0.2f, 8.0f, 4, 0.55f, 21u, 3));
    n.deformers.push_back(Deformer::twist(0.4f));
    l.sdf->insert(n);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
    clay_test::Lcg rng(1193);
    scene::Tape a = scene::compile_document(doc), b = scene::compile_document(*back);
    for (int i = 0; i < 200; ++i) {
        const cfloat3 p = rng.vec3(-2, 2);
        CHECK(b.eval(p).d == doctest::Approx(a.eval(p).d).epsilon(1e-5));
    }
}
