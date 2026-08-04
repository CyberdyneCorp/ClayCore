#include <doctest/doctest.h>

#include <vector>

#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Profile-driven modelling through the tape (add-profile-lifts).

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using scene::Profile;

namespace {

scene::Document lift_doc(scene::Prim prim, const Profile& profile,
                         const std::vector<cfloat2>& points = {},
                         cfloat3 pos = cf3(0, 0, 0)) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(prim, pos);
    n.profile = profile;
    n.profile_points = points;
    l.sdf->insert(n);
    return doc;
}

// an L-shaped concave polygon (the notch is outside)
std::vector<cfloat2> ell_polygon() {
    return {cf2(-1, -1), cf2(1, -1), cf2(1, 0), cf2(0, 0), cf2(0, 1), cf2(-1, 1)};
}

}  // namespace

TEST_CASE("extruded circle equals a capped cylinder") {
    scene::Tape tape = scene::compile_document(
        lift_doc(scene::Prim::extrude(0.7f), Profile::circle(0.55f)));
    clay_test::Lcg rng(1301);
    for (int i = 0; i < 600; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        // the lift extrudes along Z; the cylinder primitive is authored along Y
        float expected = sd_capped_cylinder(cf3(p.x, p.z, p.y), 0.55f, 0.7f);
        CHECK(tape.eval(p).d == doctest::Approx(expected).epsilon(1e-5));
    }
}

TEST_CASE("revolved circle equals a torus") {
    scene::Tape tape = scene::compile_document(
        lift_doc(scene::Prim::revolve(1.3f), Profile::circle(0.35f)));
    clay_test::Lcg rng(1302);
    for (int i = 0; i < 600; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(tape.eval(p).d == doctest::Approx(sd_torus(p, 1.3f, 0.35f)).epsilon(1e-5));
    }
}

TEST_CASE("every closed profile lifts and matches its 2D kernel") {
    struct Case {
        const char* name;
        Profile profile;
    };
    const Case cases[] = {
        {"circle", Profile::circle(0.6f)},
        {"box", Profile::box(0.5f, 0.35f)},
        {"hexagon", Profile::hexagon(0.5f)},
        {"triangle", Profile::triangle(0.5f)},
        {"trapezoid", Profile::trapezoid(0.6f, 0.3f, 0.4f)},
        {"vesica", Profile::vesica(0.7f, 0.3f)},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        scene::Tape tape =
            scene::compile_document(lift_doc(scene::Prim::extrude(0.5f), c.profile));
        clay_test::Lcg rng(1303);
        for (int i = 0; i < 250; ++i) {
            cfloat3 p = rng.vec3(-1.5f, 1.5f);
            const float* q = c.profile.params;
            float d2 = 0.0f;
            switch (c.profile.type) {
                case cprofile_circle: d2 = sd_circle2(cf2(p.x, p.y), q[0]); break;
                case cprofile_box: d2 = sd_box2(cf2(p.x, p.y), cf2(q[0], q[1])); break;
                case cprofile_hexagon: d2 = sd_hexagon2(cf2(p.x, p.y), q[0]); break;
                case cprofile_triangle:
                    d2 = sd_equilateral_triangle2(cf2(p.x, p.y), q[0]);
                    break;
                case cprofile_trapezoid:
                    d2 = sd_trapezoid2(cf2(p.x, p.y), q[0], q[1], q[2]);
                    break;
                default: d2 = sd_vesica2(cf2(p.x, p.y), q[0], q[1]); break;
            }
            CHECK(tape.eval(p).d == doctest::Approx(cop_extrude(d2, p.z, 0.5f)).epsilon(1e-5));
        }
    }
}

TEST_CASE("extruded concave polygon keeps the even-odd sign rule") {
    std::vector<cfloat2> poly = ell_polygon();
    scene::Tape tape = scene::compile_document(
        lift_doc(scene::Prim::extrude(0.4f), Profile::polygon(), poly));

    // inside the solid arm, and inside the notch (which is OUTSIDE the shape)
    CHECK(tape.eval(cf3(-0.5f, -0.5f, 0.0f)).d < 0.0f);
    CHECK(tape.eval(cf3(-0.5f, 0.5f, 0.0f)).d < 0.0f);
    CHECK(tape.eval(cf3(0.5f, 0.5f, 0.0f)).d > 0.0f);   // the notch
    CHECK(tape.eval(cf3(0.0f, 0.0f, 1.0f)).d > 0.0f);   // beyond the depth
    CHECK(tape.eval(cf3(3.0f, 0.0f, 0.0f)).d > 0.0f);   // outside entirely

    // matches the raw polygon kernel lifted by hand
    clay_test::Lcg rng(1304);
    std::vector<float> raw;
    for (const cfloat2& v : poly) {
        raw.push_back(v.x);
        raw.push_back(v.y);
    }
    for (int i = 0; i < 400; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        float d2 = sd_polygon2_raw(raw.data(), static_cast<int>(poly.size()), cf2(p.x, p.y));
        CHECK(tape.eval(p).d == doctest::Approx(cop_extrude(d2, p.z, 0.4f)).epsilon(1e-5));
    }
}

TEST_CASE("revolved polygon profile sweeps the full circle") {
    // a small square profile offset from the axis: a square-section ring
    std::vector<cfloat2> square = {cf2(-0.2f, -0.2f), cf2(0.2f, -0.2f), cf2(0.2f, 0.2f),
                                   cf2(-0.2f, 0.2f)};
    scene::Tape tape = scene::compile_document(
        lift_doc(scene::Prim::revolve(1.0f), Profile::polygon(), square));
    // solid all the way around the ring, hollow at the centre
    for (float angle : {0.0f, 1.2f, 2.4f, 3.9f, 5.6f}) {
        cfloat3 p = cf3(ccos(angle), 0.0f, csin(angle));
        CHECK(tape.eval(p).d < 0.0f);
    }
    CHECK(tape.eval(cf3(0, 0, 0)).d > 0.0f);
}

TEST_CASE("lifts of exact profiles keep the tape exact") {
    scene::Tape tape = scene::compile_document(
        lift_doc(scene::Prim::revolve(1.0f), Profile::circle(0.3f)));
    CHECK(tape.info.is_exact);
    CHECK(tape.safe_step_scale() == doctest::Approx(1.0f));
    clay_test::check_conservative_steps([&](cfloat3 p) { return tape.eval(p).d; }, 1.0f, 3.0f,
                                        500, 1305);
}

TEST_CASE("lifted items respect their influence bounds") {
    const float band = 0.15f;
    struct Case {
        const char* name;
        scene::Prim prim;
        Profile profile;
        std::vector<cfloat2> points;
    };
    std::vector<Case> cases = {
        {"extrude-circle", scene::Prim::extrude(0.5f), Profile::circle(0.4f), {}},
        {"extrude-polygon", scene::Prim::extrude(0.3f), Profile::polygon(), ell_polygon()},
        {"revolve-circle", scene::Prim::revolve(0.9f), Profile::circle(0.25f), {}},
        {"revolve-box", scene::Prim::revolve(0.8f), Profile::box(0.2f, 0.3f), {}},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(scene::Prim::sphere(1.0f), cf3(0, 0, 0)));
        scene::Node lift = item(c.prim, cf3(2.6f, 0.2f, 0.1f), scene::Op::Add,
                                scene::Blend{scene::BlendProfile::Quadratic, 0.05f});
        lift.profile = c.profile;
        lift.profile_points = c.points;
        scene::NodeId id = l.sdf->insert(lift);

        math::Aabb bound = scene::item_influence_bound(*l.sdf->find(id), l).dilated(band);
        scene::Tape full = scene::compile_document(doc);
        l.sdf->remove(id);
        scene::Tape without = scene::compile_document(doc);

        clay_test::Lcg rng(1306);
        int outside = 0;
        for (int i = 0; i < 3000; ++i) {
            cfloat3 p = rng.vec3(-6, 6);
            if (bound.contains(p)) continue;
            ++outside;
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(without.eval(p).d, -band, band));
        }
        CHECK(outside > 500);
    }
}

TEST_CASE("per-brick culled tapes stay identical with lifted items") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node ring = item(scene::Prim::revolve(1.0f), cf3(0, 0, 0));
    ring.profile = Profile::circle(0.25f);
    l.sdf->insert(ring);
    scene::Node bar = item(scene::Prim::extrude(0.6f), cf3(1.6f, 0.4f, 0), scene::Op::Add,
                           scene::Blend{scene::BlendProfile::Quadratic, 0.07f});
    bar.profile = Profile::polygon();
    bar.profile_points = ell_polygon();
    l.sdf->insert(bar);

    scene::Tape full = scene::compile_document(doc);
    const float band = 0.1f;
    clay_test::Lcg rng(1307);
    for (int b = 0; b < 25; ++b) {
        cfloat3 corner = rng.vec3(-3, 3);
        math::Aabb brick{corner, corner + cf3(0.3f, 0.3f, 0.3f)};
        scene::CullRegion cull{brick.dilated(band)};
        scene::Tape culled = scene::compile_document(doc, &cull);
        for (int i = 0; i < 80; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            CHECK(cclamp(full.eval(p).d, -band, band) ==
                  cclamp(culled.eval(p).d, -band, band));
        }
    }
}

TEST_CASE("lifted items match the reference tree evaluator and round-trip") {
    scene::Document doc = lift_doc(scene::Prim::extrude(0.45f), Profile::polygon(),
                                   ell_polygon(), cf3(0.3f, -0.2f, 0.1f));
    scene::Tape tape = scene::compile_document(doc);
    clay_test::Lcg rng(1308);
    for (int i = 0; i < 400; ++i) {
        cfloat3 p = rng.vec3(-2.5f, 2.5f);
        CHECK(tape.eval(p).d ==
              doctest::Approx(clay_test::ref_eval_document(doc, p).d).epsilon(1e-5));
    }

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
    scene::Tape reloaded = scene::compile_document(*back);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-2.5f, 2.5f);
        CHECK(tape.eval(p).d == reloaded.eval(p).d);  // bit-identical
    }
}
