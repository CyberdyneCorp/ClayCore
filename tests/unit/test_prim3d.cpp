#include <doctest/doctest.h>

#include <cmath>

#include "clay/kernel/prim3d.h"
#include "kernel_utils.h"

using namespace clay::kernel;
using clay_test::check_conservative_steps;
using clay_test::check_lipschitz;

namespace {
constexpr float kTol = 1e-5f;
}

TEST_CASE("sphere: known distances") {
    CHECK(sd_sphere(cf3(2, 0, 0), 1.0f) == doctest::Approx(1.0f).epsilon(kTol));
    CHECK(sd_sphere(cf3(0.5f, 0, 0), 1.0f) == doctest::Approx(-0.5f).epsilon(kTol));
    CHECK(sd_sphere(cf3(0, 0, 0), 1.0f) == doctest::Approx(-1.0f).epsilon(kTol));
}

TEST_CASE("box: known distances") {
    cfloat3 b = cf3(1, 1, 1);
    CHECK(sd_box(cf3(2, 0, 0), b) == doctest::Approx(1.0f).epsilon(kTol));
    CHECK(sd_box(cf3(2, 2, 2), b) == doctest::Approx(csqrt(3.0f)).epsilon(kTol));
    CHECK(sd_box(cf3(0, 0, 0), b) == doctest::Approx(-1.0f).epsilon(kTol));
    CHECK(sd_box(cf3(0.5f, 0, 0), b) == doctest::Approx(-0.5f).epsilon(kTol));
}

TEST_CASE("rounded box equals box dilated by r") {
    clay_test::Lcg rng(11);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        float r = 0.2f;
        // round box with half-extent b has the same surface as box(b-r) + r
        float a = sd_round_box(p, cf3(1, 1, 1), r);
        float e = sd_box(p, cf3(1.0f - r, 1.0f - r, 1.0f - r)) - r;
        CHECK(a == doctest::Approx(e).epsilon(1e-4));
    }
}

TEST_CASE("torus: known distances") {
    CHECK(sd_torus(cf3(2, 0, 0), 2.0f, 0.5f) == doctest::Approx(-0.5f).epsilon(kTol));
    CHECK(sd_torus(cf3(3, 0, 0), 2.0f, 0.5f) == doctest::Approx(0.5f).epsilon(kTol));
    CHECK(sd_torus(cf3(0, 0, 0), 2.0f, 0.5f) == doctest::Approx(1.5f).epsilon(kTol));
}

TEST_CASE("capsule: known distances") {
    cfloat3 a = cf3(0, 0, 0), b = cf3(0, 1, 0);
    CHECK(sd_capsule(cf3(0, 2, 0), a, b, 0.25f) == doctest::Approx(0.75f).epsilon(kTol));
    CHECK(sd_capsule(cf3(1, 0.5f, 0), a, b, 0.25f) == doctest::Approx(0.75f).epsilon(kTol));
    CHECK(sd_capsule(cf3(0, 0.5f, 0), a, b, 0.25f) == doctest::Approx(-0.25f).epsilon(kTol));
}

TEST_CASE("capped cylinder: known distances and AB equivalence") {
    CHECK(sd_capped_cylinder(cf3(2, 0, 0), 1.0f, 1.0f) == doctest::Approx(1.0f).epsilon(kTol));
    CHECK(sd_capped_cylinder(cf3(0, 2, 0), 1.0f, 1.0f) == doctest::Approx(1.0f).epsilon(kTol));
    CHECK(sd_capped_cylinder(cf3(2, 2, 0), 1.0f, 1.0f) ==
          doctest::Approx(csqrt(2.0f)).epsilon(kTol));
    CHECK(sd_capped_cylinder(cf3(0, 0, 0), 1.0f, 1.0f) == doctest::Approx(-1.0f).epsilon(kTol));

    clay_test::Lcg rng(12);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        float v = sd_capped_cylinder(p, 1.0f, 1.0f);
        float ab = sd_capped_cylinder_ab(p, cf3(0, -1, 0), cf3(0, 1, 0), 1.0f);
        CHECK(v == doctest::Approx(ab).epsilon(1e-4));
    }
}

TEST_CASE("cone: apex distance") {
    cfloat2 sc = cf2(csin(0.5f), ccos(0.5f));
    CHECK(sd_cone(cf3(0, 1, 0), sc, 1.0f) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("round cone: cap distances") {
    CHECK(sd_round_cone(cf3(0, -1, 0), 0.5f, 0.25f, 1.0f) == doctest::Approx(0.5f).epsilon(kTol));
    CHECK(sd_round_cone(cf3(0, 2, 0), 0.5f, 0.25f, 1.0f) == doctest::Approx(0.75f).epsilon(kTol));

    // AB variant with same endpoints matches
    clay_test::Lcg rng(13);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        float v = sd_round_cone(p, 0.5f, 0.25f, 1.0f);
        float ab = sd_round_cone_ab(p, cf3(0, 0, 0), cf3(0, 1, 0), 0.5f, 0.25f);
        CHECK(v == doctest::Approx(ab).epsilon(1e-4));
    }
}

TEST_CASE("round cone: one end containing the other is the larger sphere, not NaN") {
    // |r1 - r2| > h leaves no conical flank. The flank formula took csqrt of a
    // negative radicand and returned NaN, and every combine op propagates NaN,
    // so a single such item turned a whole document into NaN. A tapered
    // two-point stroke reaches the ab form the same way.
    clay_test::Lcg rng(4241);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);

        // base sphere swallows the tip
        float big_base = sd_round_cone(p, 1.0f, 0.05f, 0.1f);
        CHECK(std::isfinite(big_base));
        CHECK(big_base == doctest::Approx(clength(p) - 1.0f).epsilon(1e-4));

        // tip sphere swallows the base
        float big_tip = sd_round_cone(p, 0.05f, 1.0f, 0.1f);
        CHECK(std::isfinite(big_tip));
        CHECK(big_tip == doctest::Approx(clength(p - cf3(0, 0.1f, 0)) - 1.0f).epsilon(1e-4));

        // the ab form agrees, and survives coincident endpoints
        float ab = sd_round_cone_ab(p, cf3(0, 0, 0), cf3(0, 0.1f, 0), 1.0f, 0.05f);
        CHECK(std::isfinite(ab));
        CHECK(ab == doctest::Approx(big_base).epsilon(1e-4));

        float degenerate = sd_round_cone_ab(p, cf3(0, 0, 0), cf3(0, 0, 0), 0.5f, 0.5f);
        CHECK(std::isfinite(degenerate));
        CHECK(degenerate == doctest::Approx(clength(p) - 0.5f).epsilon(1e-4));
    }

    // and the well-formed cone is untouched
    CHECK(sd_round_cone(cf3(0, -1, 0), 0.5f, 0.25f, 1.0f) == doctest::Approx(0.5f).epsilon(kTol));
}

TEST_CASE("plane: signed distance") {
    CHECK(sd_plane(cf3(0, 3, 0), cf3(0, 1, 0), 0.0f) == doctest::Approx(3.0f).epsilon(kTol));
    CHECK(sd_plane(cf3(0, -2, 0), cf3(0, 1, 0), 0.0f) == doctest::Approx(-2.0f).epsilon(kTol));
}

TEST_CASE("octahedron: exact face distance") {
    // interior face distance from (0.5, 0, 0) to plane x+y+z=1 is (0.5-1)/sqrt(3)
    CHECK(sd_octahedron(cf3(0.5f, 0, 0), 1.0f) ==
          doctest::Approx(-0.5f * 0.57735027f).epsilon(1e-4));
    CHECK(sd_octahedron(cf3(2, 0, 0), 1.0f) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("tetrahedron: center is inside") {
    CHECK(sd_tetrahedron(cf3(0, 0, 0), 1.0f) == doctest::Approx(-0.57735027f).epsilon(1e-4));
}

TEST_CASE("ellipsoid bound is exact on axes") {
    cfloat3 r = cf3(1, 2, 3);
    CHECK(sd_ellipsoid_bound(cf3(2, 0, 0), r) == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(sd_ellipsoid_bound(cf3(0, 4, 0), r) == doctest::Approx(2.0f).epsilon(1e-3));
}

TEST_CASE("exact primitives: conservative steps and 1-Lipschitz") {
    auto run = [](auto f, std::uint64_t seed) {
        check_conservative_steps(f, 1.0f, 3.0f, 300, seed);
        check_lipschitz(f, 1.0f, 3.0f, 300, seed + 1);
    };
    run([](cfloat3 p) { return sd_sphere(p, 1.0f); }, 100);
    run([](cfloat3 p) { return sd_box(p, cf3(1, 0.7f, 0.4f)); }, 101);
    run([](cfloat3 p) { return sd_round_box(p, cf3(1, 0.7f, 0.4f), 0.1f); }, 102);
    run([](cfloat3 p) { return sd_box_frame(p, cf3(1, 0.8f, 0.6f), 0.1f); }, 103);
    run([](cfloat3 p) { return sd_torus(p, 1.5f, 0.4f); }, 104);
    run([](cfloat3 p) { return sd_capped_torus(p, cf2(csin(1.0f), ccos(1.0f)), 1.0f, 0.3f); },
        105);
    run([](cfloat3 p) { return sd_link(p, 0.5f, 0.8f, 0.2f); }, 106);
    run([](cfloat3 p) { return sd_capsule(p, cf3(-0.5f, 0, 0), cf3(0.5f, 0.3f, 0), 0.4f); }, 107);
    run([](cfloat3 p) { return sd_capped_cylinder(p, 0.8f, 1.0f); }, 108);
    run([](cfloat3 p) { return sd_rounded_cylinder(p, 0.8f, 0.2f, 1.0f); }, 109);
    run([](cfloat3 p) { return sd_cone(p, cf2(csin(0.6f), ccos(0.6f)), 1.5f); }, 110);
    run([](cfloat3 p) { return sd_capped_cone(p, 1.0f, 0.8f, 0.3f); }, 111);
    run([](cfloat3 p) { return sd_round_cone(p, 0.5f, 0.2f, 1.2f); }, 112);
    run([](cfloat3 p) { return sd_hex_prism(p, cf2(0.8f, 0.5f)); }, 113);
    run([](cfloat3 p) { return sd_octahedron(p, 1.0f); }, 114);
    run([](cfloat3 p) { return sd_pyramid(p, 1.0f); }, 115);
    run([](cfloat3 p) { return sd_cut_sphere(p, 1.0f, 0.3f); }, 116);
    run([](cfloat3 p) { return sd_cut_hollow_sphere(p, 1.0f, 0.3f, 0.1f); }, 117);
    run([](cfloat3 p) { return sd_solid_angle(p, cf2(csin(0.8f), ccos(0.8f)), 1.0f); }, 118);
    run([](cfloat3 p) { return sd_tetrahedron(p, 0.8f); }, 119);
    run([](cfloat3 p) { return sd_dodecahedron(p, 0.8f); }, 120);
    run([](cfloat3 p) { return sd_icosahedron(p, 0.8f); }, 121);
}

TEST_CASE("bound primitives: conservative steps (never overestimate)") {
    check_conservative_steps([](cfloat3 p) { return sd_ellipsoid_bound(p, cf3(1, 0.5f, 2)); },
                             1.0f, 3.0f, 300, 200);
    check_conservative_steps([](cfloat3 p) { return sd_tri_prism_bound(p, cf2(0.8f, 0.5f)); },
                             1.0f, 3.0f, 300, 201);
    check_conservative_steps([](cfloat3 p) { return sd_octahedron_bound(p, 1.0f); }, 1.0f, 3.0f,
                             300, 202);
    check_conservative_steps([](cfloat3 p) { return sd_lnorm_sphere_bound(p, 1.0f, 4.0f); }, 1.0f,
                             3.0f, 300, 203);
}
