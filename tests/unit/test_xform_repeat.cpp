#include <doctest/doctest.h>

#include "clay/kernel/prim3d.h"
#include "clay/kernel/repeat.h"
#include "clay/kernel/xform.h"
#include "kernel_utils.h"

using namespace clay::kernel;

TEST_CASE("uniform scale is exact") {
    clay_test::Lcg rng(51);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        float s = rng.range(0.5f, 3.0f);
        float d = cscale_dist(sd_sphere(cscale_point(p, s), 1.0f), s);
        CHECK(d == doctest::Approx(sd_sphere(p, s)).epsilon(1e-4));
    }
}

TEST_CASE("non-uniform scale is a conservative bound") {
    clay_test::Lcg rng(52);
    cfloat3 s = cf3(2.0f, 1.0f, 0.5f);
    auto scaled_box = [&](cfloat3 p) {
        return cscale_nu_dist(sd_box(cscale_nu_point(p, s), cf3(1, 1, 1)), s);
    };
    // surface is the scaled box surface; the field must be a safe bound
    clay_test::check_conservative_steps(scaled_box, 1.0f, 4.0f, 400, 52);
}

TEST_CASE("elongation of a sphere equals a capsule") {
    clay_test::Lcg rng(53);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        float corr = 0.0f;
        cfloat3 q = celongate_point(p, cf3(0, 0.5f, 0), &corr);
        float d = sd_sphere(q, 0.3f) + corr;
        float e = sd_capsule(p, cf3(0, -0.5f, 0), cf3(0, 0.5f, 0), 0.3f);
        CHECK(d == doctest::Approx(e).epsilon(1e-4));
    }
}

TEST_CASE("mirror fold: symmetric field") {
    clay_test::Lcg rng(54);
    auto f = [](cfloat3 p) { return sd_sphere(csym_x(p) - cf3(1, 0, 0), 0.4f); };
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(f(p) == doctest::Approx(f(cf3(-p.x, p.y, p.z))).epsilon(1e-6));
    }
}

TEST_CASE("plane mirror fold matches axis fold for the X plane") {
    clay_test::Lcg rng(55);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        cfloat3 a = cmirror_plane(p, cf3(1, 0, 0));
        cfloat3 b = csym_x(p);
        CHECK(a.x == doctest::Approx(b.x).epsilon(1e-6));
        CHECK(a.y == doctest::Approx(b.y).epsilon(1e-6));
        CHECK(a.z == doctest::Approx(b.z).epsilon(1e-6));
    }
}

TEST_CASE("round and onion") {
    // dilating a sphere r by t equals a sphere r+t
    CHECK(cop_round(sd_sphere(cf3(3, 0, 0), 1.0f), 0.5f) ==
          doctest::Approx(sd_sphere(cf3(3, 0, 0), 1.5f)).epsilon(1e-6));
    // onion: shell wall centered on the surface
    CHECK(cop_onion(sd_sphere(cf3(1, 0, 0), 1.0f), 0.1f) == doctest::Approx(-0.1f).epsilon(1e-6));
    CHECK(cop_onion(sd_sphere(cf3(0, 0, 0), 1.0f), 0.1f) == doctest::Approx(0.9f).epsilon(1e-6));
}

TEST_CASE("infinite repetition is periodic") {
    clay_test::Lcg rng(56);
    cfloat3 s = cf3(2, 2, 2);
    auto f = [&](cfloat3 p) { return sd_sphere(crep_inf_point(p, s), 0.4f); };
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(f(p) == doctest::Approx(f(p + cf3(2, 0, 0))).epsilon(1e-4));
        CHECK(f(p) == doctest::Approx(f(p + cf3(0, -4, 2))).epsilon(1e-4));
    }
}

TEST_CASE("finite repetition matches brute force over copies") {
    clay_test::Lcg rng(57);
    float s = 2.0f;
    cfloat3 l = cf3(2, 1, 0);  // 5 x 3 x 1 grid
    auto rep = [&](cfloat3 p) { return sd_sphere(crep_lim_point(p, s, l), 0.4f); };
    auto brute = [&](cfloat3 p) {
        float best = 3.4e38f;
        for (int x = -2; x <= 2; ++x)
            for (int y = -1; y <= 1; ++y) {
                cfloat3 c = cf3(s * (float)x, s * (float)y, 0.0f);
                best = cmin(best, sd_sphere(p - c, 0.4f));
            }
        return best;
    };
    for (int i = 0; i < 400; ++i) {
        cfloat3 p = rng.vec3(-7, 7);
        // primitive (r=0.4) fits its half-cell (1.0): repetition is exact
        CHECK(rep(p) == doctest::Approx(brute(p)).epsilon(1e-4));
    }
}

TEST_CASE("radial repetition: O(2) evaluation matches brute force") {
    clay_test::Lcg rng(58);
    int count = 8;
    float R = 2.0f;
    auto item = [&](cfloat3 q) { return sd_sphere(q - cf3(R, 0, 0), 0.5f); };
    auto radial2 = [&](cfloat3 p) {
        float d0 = item(crep_radial_point(p, count, 0));
        float d1 = item(crep_radial_point(p, count, crep_radial_neighbor(p, count)));
        return cmin(d0, d1);
    };
    auto brute = [&](cfloat3 p) {
        float best = 3.4e38f;
        for (int i = 0; i < count; ++i) {
            float a = 6.2831853f * (float)i / (float)count;
            float c = ccos(a), sn = csin(a);
            cfloat3 center = cf3(R * c, 0.0f, R * sn);
            best = cmin(best, sd_sphere(p - center, 0.5f));
        }
        return best;
    };
    for (int i = 0; i < 400; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        CHECK(radial2(p) == doctest::Approx(brute(p)).epsilon(1e-3));
    }
}
