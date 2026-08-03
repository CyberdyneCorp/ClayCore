#include <doctest/doctest.h>

#include "clay/kernel/deform.h"
#include "clay/kernel/exactness.h"
#include "clay/kernel/prim3d.h"
#include "kernel_utils.h"

using namespace clay::kernel;
using clay_test::check_conservative_steps;

TEST_CASE("field info combinators propagate classification") {
    CFieldInfo e = cfi_exact();
    CHECK(e.is_exact);
    CHECK(csafe_step_scale(e) == doctest::Approx(1.0f));

    // exact ∪ exact stays exact; anything with a bound loses exactness
    CHECK(cfi_boolean(e, e).is_exact);
    CHECK_FALSE(cfi_boolean(e, cfi_bound()).is_exact);
    CHECK_FALSE(cfi_smooth_blend(e, e).is_exact);
    // smooth blends stay safe at full steps
    CHECK(csafe_step_scale(cfi_smooth_blend(e, e)) == doctest::Approx(1.0f));

    // twist wrapped in a union: worst Lipschitz wins
    CFieldInfo tw = cfi_twist(e, 1.2f, 1.5f);
    CHECK_FALSE(tw.is_exact);
    CHECK(tw.lipschitz == doctest::Approx(1.0f + 1.2f * 1.5f));
    CFieldInfo u = cfi_boolean(tw, e);
    CHECK(u.lipschitz == doctest::Approx(tw.lipschitz));
    CHECK(csafe_step_scale(u) == doctest::Approx(1.0f / tw.lipschitz));

    // displacement adds Lipschitz constants
    CHECK(cfi_displace(e, 0.5f).lipschitz == doctest::Approx(1.5f));
}

TEST_CASE("twisted box: tracked step scale is conservative") {
    float k = 1.0f;
    cfloat3 b = cf3(0.8f, 1.0f, 0.8f);
    float radius_bound = clength(cf2(b.x, b.z));
    auto f = [&](cfloat3 p) { return sd_box(ctwist_point(p, k), b); };
    CFieldInfo info = cfi_twist(cfi_exact(), k, radius_bound);
    check_conservative_steps(f, csafe_step_scale(info), 3.0f, 500, 71);
}

TEST_CASE("bent capsule: tracked step scale is conservative") {
    float k = 0.6f;
    auto f = [&](cfloat3 p) {
        return sd_capsule(cbend_point(p, k), cf3(-1.5f, 0, 0), cf3(1.5f, 0, 0), 0.3f);
    };
    CFieldInfo info = cfi_bend(cfi_exact(), k, 2.0f);
    check_conservative_steps(f, csafe_step_scale(info), 3.0f, 500, 72);
}

TEST_CASE("tapered cylinder: tracked step scale is conservative") {
    float s0 = 1.0f, s1 = 0.4f;
    auto f = [&](cfloat3 p) {
        cfloat3 q = ctaper_point(p, -1.0f, 1.0f, s0, s1, ease_linear);
        return ctaper_dist(sd_capped_cylinder(q, 0.6f, 1.0f), s0, s1);
    };
    CFieldInfo info = cfi_taper(cfi_exact(), cmin(s0, s1), cmax(s0, s1), 2.0f, 0.6f / cmin(s0, s1));
    check_conservative_steps(f, csafe_step_scale(info), 3.0f, 500, 73);
}

TEST_CASE("displaced sphere: tracked step scale is conservative") {
    // g = 0.05 sin(5x) -> Lipschitz(g) = 0.25
    auto f = [](cfloat3 p) {
        return cdisplace_dist(sd_sphere(p, 1.0f), 0.05f * csin(5.0f * p.x));
    };
    CFieldInfo info = cfi_displace(cfi_exact(), 0.25f);
    check_conservative_steps(f, csafe_step_scale(info), 2.5f, 500, 74);
}

TEST_CASE("bend_linear: displacement over segment is conservative") {
    cfloat3 a = cf3(0, -1, 0), b = cf3(0, 1, 0), v = cf3(0.8f, 0, 0);
    auto f = [&](cfloat3 p) {
        return sd_sphere(cbend_linear_point(p, a, b, v, ease_smoothstep), 0.5f);
    };
    // ease slope bound for smoothstep is 1.5 -> extra stretch |v|/len * 1.5
    CFieldInfo info = cfi_bend_linear(cfi_exact(), clength(v) * 1.5f, clength(b - a));
    check_conservative_steps(f, csafe_step_scale(info), 3.0f, 500, 75);
}

TEST_CASE("transition weight fields are in [0,1] and monotone along the axis") {
    clay_test::Lcg rng(76);
    cfloat3 a = cf3(0, -1, 0), b = cf3(0, 1, 0);
    float prev = -1.0f;
    for (int i = 0; i <= 50; ++i) {
        float y = -2.0f + 4.0f * static_cast<float>(i) / 50.0f;
        float w = ctransition_linear_weight(cf3(0.3f, y, -0.2f), a, b, ease_smoothstep);
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
        CHECK(w >= prev - 1e-6f);
        prev = w;
    }
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        float w = ctransition_radial_weight(p, 0.5f, 2.0f, ease_in_out_cubic);
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
    }
}

TEST_CASE("wrap_around unwraps to the flat interval") {
    // a point on the wrap cylinder at angle 0 maps to the interval center
    float x0 = -1.0f, x1 = 1.0f;
    float r = (x1 - x0) / 6.2831853f;
    cfloat3 q = cwrap_around_point(cf3(r, 0, 0.3f), x0, x1);
    CHECK(q.x == doctest::Approx(0.0f).epsilon(1e-4));   // interval center
    CHECK(q.y == doctest::Approx(0.0f).epsilon(1e-4));   // on the cylinder
    CHECK(q.z == doctest::Approx(0.3f).epsilon(1e-6));   // z untouched
}
