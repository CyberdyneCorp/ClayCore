// Surface relief (sdf-kernels spec, add-surface-relief).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/kernel/exactness.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;

namespace {

// A ball, optionally with a relief region on top of it.
scene::Document ball_with(scene::Op op, float amplitude = 0.12f, float width = 0.25f,
                          bool include_region = true) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node ball;
    ball.prim = scene::Prim::sphere(0.7f);
    l.sdf->insert(std::move(ball));
    if (include_region) {
        scene::Node region;
        region.prim = scene::Prim::sphere(0.35f);
        region.xform.position = cf3(0, 0.7f, 0);
        region.op = op;
        region.blend = scene::Blend{scene::BlendProfile::Quadratic, amplitude};
        region.rounding = width;
        l.sdf->insert(std::move(region));
    }
    return doc;
}

scene::Document plain_ball() { return ball_with(scene::Op::Add, 0.0f, 0.0f, false); }

// Where the surface sits on the +Y axis.
float top(const scene::Tape& t) {
    for (float y = 0.2f; y < 1.4f; y += 0.001f)
        if (t.eval(cf3(0, y, 0)).d > 0.0f) return y;
    return -1.0f;
}

float steepest_slope(const scene::Tape& t) {
    const float h = 1e-3f;
    float worst = 0.0f;
    for (float x = -0.9f; x <= 0.9f; x += 0.011f)
        for (float y = -0.2f; y <= 1.1f; y += 0.013f) {
            const float gx = (t.eval(cf3(x + h, y, 0)).d - t.eval(cf3(x - h, y, 0)).d) / (2 * h);
            const float gy = (t.eval(cf3(x, y + h, 0)).d - t.eval(cf3(x, y - h, 0)).d) / (2 * h);
            worst = std::max(worst, std::sqrt(gx * gx + gy * gy));
        }
    return worst;
}

}  // namespace

TEST_CASE("relief: it displaces the surface accumulated before it") {
    const float amplitude = 0.12f;
    const float base = top(scene::compile_document(plain_ball()));
    REQUIRE(base == doctest::Approx(0.7f).epsilon(0.01));

    const float up = top(scene::compile_document(ball_with(scene::Op::Relief, amplitude)));
    const float in = top(scene::compile_document(ball_with(scene::Op::Incise, amplitude)));

    INFO("surface: " << in << " (incise) < " << base << " (plain) < " << up << " (relief)");
    // Offsetting a distance field moves its isosurface along the field's own
    // gradient — the surface normal — by exactly the offset. Not approximately.
    CHECK(up - base == doctest::Approx(amplitude).epsilon(0.05));
    CHECK(base - in == doctest::Approx(amplitude).epsilon(0.05));
}

TEST_CASE("relief: the item is a region, not a shape") {
    // The whole distinction. A relief item alone contributes no surface: it has
    // nothing to displace, and it is not itself geometry.
    scene::Document solo;
    scene::Layer& l = solo.add_sdf_layer("l");
    scene::Node region;
    region.prim = scene::Prim::sphere(0.35f);
    region.op = scene::Op::Relief;
    region.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.12f};
    region.rounding = 0.25f;
    l.sdf->insert(std::move(region));

    scene::Tape tape = scene::compile_document(solo);
    // Nothing anywhere: no surface was accumulated for it to move.
    for (float x = -1.0f; x <= 1.0f; x += 0.13f)
        for (float y = -1.0f; y <= 1.0f; y += 0.17f) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(tape.eval(cf3(x, y, 0.05f)).d > 0.0f);
        }
}

TEST_CASE("relief: support really is finite") {
    // Item influence bounds and brick culling both trust this. Outside the
    // falloff the field must be identical, not merely close.
    scene::Tape with = scene::compile_document(ball_with(scene::Op::Relief, 0.12f, 0.25f));
    scene::Tape without = scene::compile_document(plain_ball());
    const kernel::cfloat3 centre = cf3(0, 0.7f, 0);
    // Radius + rounding + falloff. The rounding does DOUBLE DUTY, exactly as it
    // does for groove and tongue: it rounds the item's own field AND is the
    // falloff width, so the taper sits outside the rounded surface rather than
    // inside the raw one. Bounds already dilate by rounding + support for the
    // same reason; a reach of radius + falloff alone is the wrong number.
    const float reach = 0.35f + 0.25f + 0.25f;

    int checked = 0;
    for (float x = -1.4f; x <= 1.4f; x += 0.031f)
        for (float y = -1.4f; y <= 1.6f; y += 0.037f) {
            kernel::cfloat3 p = cf3(x, y, 0.019f);
            if (kernel::clength(p - centre) <= reach) continue;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(with.eval(p).d == doctest::Approx(without.eval(p).d));
            ++checked;
        }
    REQUIRE(checked > 500);
}

TEST_CASE("relief: the influence bound covers the rounded region, not the raw one") {
    // The bound is what brick culling drops work on, so it has to be dilated by
    // the rounding as well as the falloff — the same double duty groove and
    // tongue have. Measured out from the region rather than asserted.
    scene::Tape with = scene::compile_document(ball_with(scene::Op::Relief, 0.12f, 0.25f));
    scene::Tape without = scene::compile_document(plain_ball());
    const kernel::cfloat3 centre = cf3(0, 0.7f, 0);

    float furthest = 0.0f;
    for (float x = -1.4f; x <= 1.4f; x += 0.017f)
        for (float y = -1.4f; y <= 1.6f; y += 0.019f) {
            kernel::cfloat3 p = cf3(x, y, 0.013f);
            if (std::abs(with.eval(p).d - without.eval(p).d) > 1e-5f)
                furthest = std::max(furthest, kernel::clength(p - centre));
        }
    INFO("the region changes the field out to " << furthest
         << "; radius 0.35 + rounding 0.25 + falloff 0.25 = 0.85");
    CHECK(furthest > 0.35f + 0.25f);   // the rounding really does grow the region
    CHECK(furthest <= 0.35f + 0.5f + 1e-3f);

    // ...and that reach is inside what the item's bounds were dilated by.
    REQUIRE_FALSE(with.bounds.empty());
    for (float a = 0.0f; a < 6.28f; a += 0.21f) {
        kernel::cfloat3 p = centre + cf3(std::cos(a), std::sin(a), 0.0f) * furthest;
        CAPTURE(a);
        CHECK(p.x <= with.bounds.max.x + 1e-4f);
        CHECK(p.y <= with.bounds.max.y + 1e-4f);
        CHECK(p.x >= with.bounds.min.x - 1e-4f);
        CHECK(p.y >= with.bounds.min.y - 1e-4f);
    }
}

TEST_CASE("relief: zero amplitude changes nothing") {
    scene::Tape with = scene::compile_document(ball_with(scene::Op::Relief, 0.0f, 0.25f));
    scene::Tape without = scene::compile_document(plain_ball());
    for (float x = -1.0f; x <= 1.0f; x += 0.043f)
        for (float y = -0.2f; y <= 1.2f; y += 0.047f) {
            kernel::cfloat3 p = cf3(x, y, 0.011f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(with.eval(p).d == doctest::Approx(without.eval(p).d));
        }
}

TEST_CASE("relief: the two directions are each other's inverse") {
    // They share one kernel branch with the sign taken from the mode, which is
    // what keeps this true as either changes.
    const float base = top(scene::compile_document(plain_ball()));
    for (float amp : {0.05f, 0.12f, 0.2f}) {
        const float up = top(scene::compile_document(ball_with(scene::Op::Relief, amp)));
        const float in = top(scene::compile_document(ball_with(scene::Op::Incise, amp)));
        CAPTURE(amp);
        CHECK(up - base == doctest::Approx(base - in).epsilon(0.05));
    }
}

TEST_CASE("relief: the steepness it adds is declared, and the field meets it") {
    scene::Tape plain = scene::compile_document(plain_ball());
    CHECK(plain.info.is_exact);

    scene::Tape relieved = scene::compile_document(ball_with(scene::Op::Relief, 0.12f, 0.25f));
    CHECK_FALSE(relieved.info.is_exact);
    CHECK(relieved.info.lipschitz > plain.info.lipschitz);
    CHECK(kernel::csafe_step_scale(relieved.info) < kernel::csafe_step_scale(plain.info));

    SUBCASE("the declared bound is measured, not assumed") {
        const float measured = steepest_slope(relieved);
        INFO("declared " << relieved.info.lipschitz << ", measured " << measured);
        CHECK(measured <= relieved.info.lipschitz + 1e-2f);
    }

    SUBCASE("a deeper relief declares more") {
        float previous = kernel::csafe_step_scale(plain.info);
        for (float amp : {0.05f, 0.12f, 0.25f}) {
            const float scale = kernel::csafe_step_scale(
                scene::compile_document(ball_with(scene::Op::Relief, amp, 0.25f)).info);
            CAPTURE(amp);
            CHECK(scale <= previous);
            previous = scale;
        }
    }

    SUBCASE("and a narrower falloff declares more") {
        float previous = 1.0f;
        for (float width : {0.5f, 0.25f, 0.1f}) {
            const float scale = kernel::csafe_step_scale(
                scene::compile_document(ball_with(scene::Op::Relief, 0.12f, width)).info);
            CAPTURE(width);
            CHECK(scale <= previous);
            previous = scale;
        }
    }
}

TEST_CASE("relief: a ray still finds the displaced surface") {
    scene::Tape tape = scene::compile_document(ball_with(scene::Op::Relief, 0.12f, 0.25f));
    const float scale = kernel::csafe_step_scale(tape.info);

    float t = 0.0f;
    bool hit = false;
    for (int i = 0; i < 40000; ++i) {
        const float d = tape.eval(cf3(0, 3.0f - t, 0)).d;
        if (d < 1e-4f) {
            hit = true;
            break;
        }
        t += d * scale;
        if (t > 6.0f) break;
    }
    REQUIRE(hit);
    INFO("landed at y = " << 3.0f - t);
    CHECK(3.0f - t == doctest::Approx(0.82f).epsilon(0.0).scale(1.0).epsilon(0.03));
}

TEST_CASE("relief: any primitive can be the region") {
    // A sphere for a round brush, a box for a chisel — the region is whatever
    // primitive the item carries.
    for (int which = 0; which < 2; ++which) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node ball;
        ball.prim = scene::Prim::sphere(0.7f);
        l.sdf->insert(std::move(ball));
        scene::Node region;
        region.prim = which == 0 ? scene::Prim::sphere(0.3f)
                                 : scene::Prim::box(cf3(0.3f, 0.3f, 0.1f));
        region.xform.position = cf3(0, 0.7f, 0);
        region.op = scene::Op::Relief;
        region.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.1f};
        region.rounding = 0.2f;
        l.sdf->insert(std::move(region));

        CAPTURE(which);
        scene::Tape tape = scene::compile_document(doc);
        CHECK(top(tape) > 0.75f);  // displaced either way
    }
}

TEST_CASE("relief: it survives a round trip") {
    scene::Document doc = ball_with(scene::Op::Incise, 0.12f, 0.25f);
    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    scene::Tape before = scene::compile_document(doc);
    scene::Tape after = scene::compile_document(*back);
    for (float y = 0.2f; y <= 1.2f; y += 0.07f)
        CHECK(after.eval(cf3(0, y, 0)).d == doctest::Approx(before.eval(cf3(0, y, 0)).d));
}
