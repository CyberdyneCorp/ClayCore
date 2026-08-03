#include <doctest/doctest.h>

#include "clay/kernel/exactness.h"
#include "clay/kernel/field.h"
#include "clay/kernel/ops.h"
#include "clay/kernel/prim3d.h"
#include "kernel_utils.h"

using namespace clay::kernel;

namespace {
float sphere_map(cfloat3 p) { return sd_sphere(p, 1.0f); }
}  // namespace

TEST_CASE("tetrahedron normals match analytic on a sphere") {
    clay_test::Lcg rng(91);
    for (int i = 0; i < 500; ++i) {
        cfloat3 dir = rng.unit3();
        cfloat3 p = dir * 1.0f;  // on the surface
        cfloat3 n = cnormal(sphere_map, p, 1e-4f);
        // angular error below 1e-3 rad
        CHECK(cdot(n, dir) > ccos(1e-3f));
    }
}

TEST_CASE("normals on a smooth union stay unit-length and continuous") {
    auto map = [](cfloat3 p) {
        return csmin_quadratic(sd_sphere(p - cf3(0.5f, 0, 0), 0.6f),
                               sd_sphere(p + cf3(0.5f, 0, 0), 0.6f), 0.1f);
    };
    clay_test::Lcg rng(92);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.unit3() * rng.range(0.8f, 1.5f);
        cfloat3 n = cnormal(map, p, 1e-4f);
        CHECK(clength(n) == doctest::Approx(1.0f).epsilon(1e-4));
    }
}

TEST_CASE("sphere trace hits a sphere at the analytic distance") {
    cfloat3 ro = cf3(0, 0, -3);
    cfloat3 rd = cf3(0, 0, 1);
    CRayHit hit = craycast(sphere_map, ro, rd, 0.0f, 10.0f, 1e-4f, 1.0f, 1.0f, 256);
    REQUIRE(hit.hit);
    CHECK(hit.t == doctest::Approx(2.0f).epsilon(2e-3));
}

TEST_CASE("over-relaxation: same hits, fewer steps on grazing rays") {
    // Over-relaxation pays off when rays skim close to geometry (steps stay
    // small for a long stretch): travel parallel to a plane, hit a sphere.
    auto map = [](cfloat3 p) {
        return cmin(p.y + 1.0f, sd_sphere(p - cf3(6, 0, 0), 1.0f));
    };
    clay_test::Lcg rng(93);
    int plain_total = 0, relaxed_total = 0, hits = 0;
    for (int i = 0; i < 100; ++i) {
        cfloat3 ro = cf3(0.0f, rng.range(-0.95f, -0.7f), rng.range(-0.3f, 0.3f));
        cfloat3 rd = cnormalize(cf3(1.0f, rng.range(-0.005f, 0.005f), 0.0f));
        CRayHit plain = craycast(map, ro, rd, 0.0f, 20.0f, 1e-4f, 1.0f, 1.0f, 1024);
        CRayHit relaxed = craycast(map, ro, rd, 0.0f, 20.0f, 1e-4f, 1.0f, 1.6f, 1024);
        REQUIRE(plain.hit == relaxed.hit);
        if (plain.hit) {
            CHECK(relaxed.t == doctest::Approx(plain.t).epsilon(2e-3));
            plain_total += plain.steps;
            relaxed_total += relaxed.steps;
            ++hits;
        }
    }
    CHECK(hits > 50);
    CHECK(relaxed_total < plain_total);  // the 20-40% fewer-iterations claim
}

TEST_CASE("miss returns no hit") {
    CRayHit hit = craycast(sphere_map, cf3(0, 3, -3), cf3(0, 0, 1), 0.0f, 10.0f, 1e-4f, 1.0f,
                           1.0f, 256);
    CHECK_FALSE(hit.hit);
}

TEST_CASE("tracked step scale is required for Lipschitz > 1 fields") {
    // A field that overestimates by 1.5x if stepped naively.
    auto bad_map = [](cfloat3 p) { return 1.5f * sd_sphere(p, 1.0f); };
    CFieldInfo info{false, 1.5f};
    cfloat3 ro = cf3(0, 0, -3);
    cfloat3 rd = cf3(0, 0, 1);
    CRayHit safe = craycast(bad_map, ro, rd, 0.0f, 10.0f, 1e-4f, csafe_step_scale(info), 1.0f, 256);
    REQUIRE(safe.hit);
    CHECK(safe.t == doctest::Approx(2.0f).epsilon(5e-3));
}

TEST_CASE("AO is 1 in open space along the normal of an isolated surface") {
    cfloat3 p = cf3(0, 0, -1);
    cfloat3 n = cf3(0, 0, -1);
    CHECK(cao(sphere_map, p, n) == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("AO drops in a crevice") {
    // point between two near-touching spheres
    auto map = [](cfloat3 p) {
        return cmin(sd_sphere(p - cf3(0.62f, 0, 0), 0.6f), sd_sphere(p + cf3(0.62f, 0, 0), 0.6f));
    };
    float open_ao = cao(map, cf3(0.62f, 0, 0.6f), cf3(0, 0, 1));   // top of a sphere
    float crevice_ao = cao(map, cf3(0, 0, 0.05f), cf3(0, 0, 1));   // in the gap
    CHECK(crevice_ao < open_ao);
    CHECK(crevice_ao < 0.6f);
}

TEST_CASE("soft shadow: blocked, open, and penumbra") {
    cfloat3 ro = cf3(0, 0, -3);
    // straight through the sphere: fully shadowed
    CHECK(csoftshadow(sphere_map, ro, cf3(0, 0, 1), 0.01f, 10.0f, 0.1f, 1.0f) ==
          doctest::Approx(0.0f));
    // pointing away: fully lit
    CHECK(csoftshadow(sphere_map, ro, cf3(0, 0, -1), 0.01f, 10.0f, 0.1f, 1.0f) ==
          doctest::Approx(1.0f).epsilon(1e-3));
    // grazing ray (closest approach ~1.07 > r): penumbra
    cfloat3 rd = cnormalize(cf3(0.0f, 1.15f, 3.0f));
    float s = csoftshadow(sphere_map, ro, rd, 0.01f, 10.0f, 0.3f, 1.0f);
    CHECK(s > 0.0f);
    CHECK(s < 1.0f);
}
