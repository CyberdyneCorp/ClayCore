#include <doctest/doctest.h>

#include <vector>

#include "clay/kernel/prim2d.h"
#include "clay/math/bezier.h"
#include "kernel_utils.h"

using namespace clay::kernel;

namespace {

// Dense-sample ground-truth distance to a parametric curve.
template <typename Curve>
float dense_curve_distance(cfloat2 p, Curve c, int samples) {
    float best = 3.4e38f;
    for (int i = 0; i <= samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(samples);
        best = cmin(best, clength(p - c(t)));
    }
    return best;
}

}  // namespace

TEST_CASE("circle and box2 known values") {
    CHECK(sd_circle2(cf2(2, 0), 1.0f) == doctest::Approx(1.0f));
    CHECK(sd_box2(cf2(2, 0), cf2(1, 1)) == doctest::Approx(1.0f));
    CHECK(sd_box2(cf2(2, 2), cf2(1, 1)) == doctest::Approx(csqrt(2.0f)));
    CHECK(sd_box2(cf2(0, 0), cf2(1, 1)) == doctest::Approx(-1.0f));
}

TEST_CASE("segment distance") {
    CHECK(sd_segment2(cf2(0.5f, 1), cf2(0, 0), cf2(1, 0)) == doctest::Approx(1.0f));
    CHECK(sd_segment2(cf2(2, 0), cf2(0, 0), cf2(1, 0)) == doctest::Approx(1.0f));
}

TEST_CASE("polygon: square matches box2") {
    std::vector<cfloat2> square = {cf2(-1, -1), cf2(1, -1), cf2(1, 1), cf2(-1, 1)};
    clay_test::Lcg rng(31);
    for (int i = 0; i < 300; ++i) {
        cfloat2 p = rng.vec2(-3, 3);
        float a = sd_polygon2(square.data(), 4, p);
        float e = sd_box2(p, cf2(1, 1));
        CHECK(a == doctest::Approx(e).epsilon(1e-4));
    }
}

TEST_CASE("polygon: concave L-shape sign correctness") {
    // L-shape: the notch [0,2]x[0,2] is OUTSIDE
    std::vector<cfloat2> ell = {cf2(-2, -2), cf2(2, -2), cf2(2, 0),
                                cf2(0, 0),   cf2(0, 2),  cf2(-2, 2)};
    CHECK(sd_polygon2(ell.data(), 6, cf2(1, 1)) > 0.0f);    // in the notch
    CHECK(sd_polygon2(ell.data(), 6, cf2(-1, -1)) < 0.0f);  // solid part
    CHECK(sd_polygon2(ell.data(), 6, cf2(-1, 1)) < 0.0f);   // upper arm
    CHECK(sd_polygon2(ell.data(), 6, cf2(3, 0)) > 0.0f);    // fully outside
    // distance in the notch to the two inner edges
    CHECK(sd_polygon2(ell.data(), 6, cf2(1, 1)) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("hexagon, triangle, trapezoid, vesica: sign sanity") {
    CHECK(sd_hexagon2(cf2(0, 0), 1.0f) < 0.0f);
    CHECK(sd_hexagon2(cf2(3, 0), 1.0f) > 0.0f);
    CHECK(sd_equilateral_triangle2(cf2(0, 0), 1.0f) < 0.0f);
    CHECK(sd_equilateral_triangle2(cf2(3, 0), 1.0f) > 0.0f);
    CHECK(sd_trapezoid2(cf2(0, 0), 1.0f, 0.5f, 1.0f) < 0.0f);
    CHECK(sd_trapezoid2(cf2(4, 0), 1.0f, 0.5f, 1.0f) > 0.0f);
    CHECK(sd_vesica2(cf2(0, 0), 1.0f, 0.5f) < 0.0f);
    CHECK(sd_vesica2(cf2(3, 0), 1.0f, 0.5f) > 0.0f);
}

TEST_CASE("quadratic Bezier matches dense sampling") {
    cfloat2 A = cf2(-1, 0), B = cf2(0, 2), C = cf2(1, 0);
    auto curve = [&](float t) {
        float u = 1.0f - t;
        return A * (u * u) + B * (2.0f * u * t) + C * (t * t);
    };
    clay_test::Lcg rng(32);
    for (int i = 0; i < 200; ++i) {
        cfloat2 p = rng.vec2(-2, 3);
        float a = sd_bezier2(p, A, B, C);
        float e = dense_curve_distance(p, curve, 4000);
        CHECK(a == doctest::Approx(e).epsilon(2e-3));
    }
}

TEST_CASE("degenerate (collinear) Bezier falls back to segment") {
    cfloat2 A = cf2(0, 0), B = cf2(0.5f, 0), C = cf2(1, 0);
    CHECK(sd_bezier2(cf2(0.5f, 1), A, B, C) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("cubic Bezier via adaptive quadratic subdivision") {
    cfloat2 p0 = cf2(-1, 0), p1 = cf2(-0.5f, 2), p2 = cf2(0.5f, -2), p3 = cf2(1, 0);
    auto curve = [&](float t) {
        float u = 1.0f - t;
        return p0 * (u * u * u) + p1 * (3.0f * u * u * t) + p2 * (3.0f * u * t * t) +
               p3 * (t * t * t);
    };
    // subdivision produced more than one quadratic for this S-curve
    CHECK(clay::math::cubic_to_quadratics(p0, p1, p2, p3, 1e-3f).size() > 1);
    clay_test::Lcg rng(33);
    for (int i = 0; i < 200; ++i) {
        cfloat2 p = rng.vec2(-2, 2);
        float a = clay::math::sd_cubic_bezier(p, p0, p1, p2, p3, 1e-4f);
        float e = dense_curve_distance(p, curve, 4000);
        CHECK(a == doctest::Approx(e).epsilon(2e-3));
    }
}
