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
