#include <doctest/doctest.h>

#include "clay/kernel/ease.h"
#include "clay/kernel/lift.h"
#include "clay/kernel/prim2d.h"
#include "clay/kernel/prim3d.h"
#include "kernel_utils.h"

using namespace clay::kernel;

TEST_CASE("extruded circle equals capped cylinder (exact lift)") {
    clay_test::Lcg rng(61);
    for (int i = 0; i < 400; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        // extrude along Z; capped cylinder is authored along Y -> permute
        float d2 = sd_circle2(cf2(p.x, p.y), 0.8f);
        float lifted = cop_extrude(d2, p.z, 1.0f);
        float direct = sd_capped_cylinder(cf3(p.x, p.z, p.y), 0.8f, 1.0f);
        CHECK(lifted == doctest::Approx(direct).epsilon(1e-4));
    }
}

TEST_CASE("revolved circle equals torus (exact lift)") {
    clay_test::Lcg rng(62);
    for (int i = 0; i < 400; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        float lifted = sd_circle2(crevolve_point(p, 2.0f), 0.5f);
        float direct = sd_torus(p, 2.0f, 0.5f);
        CHECK(lifted == doctest::Approx(direct).epsilon(1e-4));
    }
}

namespace {
// The two-profile parameterisation `cop_loft` used to compute for itself. It
// lives here now because the tape computes it — with more than two profiles
// it has to bracket first, so a signature that derived it from pz could only
// ever serve exactly two.
float loft2(float d2a, float d2b, float pz, float h, int ease) {
    return cop_loft(d2a, d2b, cease(ease, cclamp((pz + h) / (2.0f * h), 0.0f, 1.0f)), pz, h);
}
}  // namespace

TEST_CASE("extrude-to with equal profiles reduces to extrude") {
    clay_test::Lcg rng(63);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        float d2 = sd_circle2(cf2(p.x, p.y), 0.8f);
        CHECK(loft2(d2, d2, p.z, 1.0f, ease_linear) ==
              doctest::Approx(cop_extrude(d2, p.z, 1.0f)).epsilon(1e-5));
    }
}

TEST_CASE("extrude-to (loft) is a conservative bound") {
    auto loft = [](cfloat3 p) {
        float bottom = sd_circle2(cf2(p.x, p.y), 1.0f);
        float top = sd_box2(cf2(p.x, p.y), cf2(0.5f, 0.5f));
        return loft2(bottom, top, p.z, 1.0f, ease_smoothstep);
    };
    clay_test::check_conservative_steps(loft, 1.0f, 3.0f, 400, 63);
}

TEST_CASE("every easing curve fixes the endpoints and clamps input") {
    for (int e = 0; e < ease_count; ++e) {
        CAPTURE(e);
        CHECK(cease(e, 0.0f) == doctest::Approx(0.0f).epsilon(1e-5));
        CHECK(cease(e, 1.0f) == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(cease(e, -2.0f) == doctest::Approx(cease(e, 0.0f)).epsilon(1e-6));
        CHECK(cease(e, 3.0f) == doctest::Approx(cease(e, 1.0f)).epsilon(1e-6));
    }
    CHECK(ease_count == 33);
}

TEST_CASE("monotone curves are monotone; overshoot curves stay bounded") {
    // back/elastic overshoot by design; bounce oscillates. All others are
    // monotone non-decreasing on [0,1].
    for (int e = 0; e < ease_count; ++e) {
        bool overshoot = (e >= ease_in_back && e <= ease_in_out_bounce);
        float prev = cease(e, 0.0f);
        for (int i = 1; i <= 100; ++i) {
            float t = static_cast<float>(i) / 100.0f;
            float v = cease(e, t);
            CAPTURE(e);
            CAPTURE(t);
            if (!overshoot) CHECK(v >= prev - 1e-6f);
            CHECK(v >= -0.6f);  // bounded overshoot
            CHECK(v <= 1.6f);
            prev = v;
        }
    }
}

TEST_CASE("named curve values") {
    CHECK(cease(ease_linear, 0.25f) == doctest::Approx(0.25f));
    CHECK(cease(ease_smoothstep, 0.5f) == doctest::Approx(0.5f));
    CHECK(cease(ease_in_quad, 0.5f) == doctest::Approx(0.25f));
    CHECK(cease(ease_out_quad, 0.5f) == doctest::Approx(0.75f));
    CHECK(cease(ease_in_out_quad, 0.5f) == doctest::Approx(0.5f));
}
