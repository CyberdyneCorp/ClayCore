#include <doctest/doctest.h>

#include "clay/kernel/ops.h"
#include "kernel_utils.h"

using namespace clay::kernel;

TEST_CASE("hard booleans") {
    CHECK(op_union(1.0f, 2.0f) == 1.0f);
    CHECK(op_subtract(-1.0f, 2.0f) == 2.0f);   // d2 minus d1
    CHECK(op_subtract(0.5f, -1.0f) == -0.5f);  // carve boundary wins
    CHECK(op_intersect(1.0f, 2.0f) == 2.0f);
    CHECK(op_xor(-1.0f, -2.0f) == 1.0f);  // inside both -> outside
}

TEST_CASE("smin rigidity: identical to min outside the support width") {
    clay_test::Lcg rng(41);
    for (int i = 0; i < 3000; ++i) {
        float k = rng.range(0.01f, 0.5f);
        float a = rng.range(-3, 3);
        // place b outside the support in either direction
        float side = rng.next01() < 0.5f ? -1.0f : 1.0f;
        float m = rng.range(0.0f, 2.0f);

        float b_quad = a + side * (csmin_quadratic_support(k) + m);
        CHECK(csmin_quadratic(a, b_quad, k) == cmin(a, b_quad));

        float b_cub = a + side * (csmin_cubic_support(k) + m);
        CHECK(csmin_cubic(a, b_cub, k) == cmin(a, b_cub));

        float b_circ = a + side * (csmin_circular_support(k) + m);
        CHECK(csmin_circular(a, b_circ, k) == doctest::Approx(cmin(a, b_circ)).epsilon(1e-6));
    }
}

TEST_CASE("chamfer rigidity in the surface region") {
    // Chamfer deviates only when max(a,b) < min(a,b)*(sqrt(2)-1) + k; for
    // non-negative min it is identical to min once |a-b| >= k.
    clay_test::Lcg rng(42);
    for (int i = 0; i < 3000; ++i) {
        float k = rng.range(0.01f, 0.5f);
        float a = rng.range(0.0f, 3.0f);
        float b = a + k + rng.range(0.0f, 2.0f);
        CHECK(cchamfer(a, b, k) == cmin(a, b));
        CHECK(cchamfer(b, a, k) == cmin(a, b));
    }
}

TEST_CASE("smins add material, never remove: min - dev <= smin <= min") {
    clay_test::Lcg rng(43);
    for (int i = 0; i < 3000; ++i) {
        float k = rng.range(0.01f, 0.5f);
        float a = rng.range(-2, 2), b = rng.range(-2, 2);
        float m = cmin(a, b);
        CHECK(csmin_quadratic(a, b, k) <= m + 1e-6f);
        CHECK(csmin_quadratic(a, b, k) >= m - csmin_quadratic_support(k));
        CHECK(csmin_cubic(a, b, k) <= m + 1e-6f);
        CHECK(csmin_circular(a, b, k) <= m + 1e-5f);
        CHECK(cchamfer(a, b, k) <= m + 1e-6f);
    }
}

TEST_CASE("k = 0 degenerates to the hard boolean") {
    CHECK(csmin_quadratic(0.5f, -1.0f, 0.0f) == -1.0f);
    CHECK(csmin_cubic(0.5f, -1.0f, 0.0f) == -1.0f);
    CHECK(csmin_circular(0.5f, -1.0f, 0.0f) == -1.0f);
}

TEST_CASE("De Morgan smooth ops approach hard ops for tiny k") {
    clay_test::Lcg rng(44);
    for (int i = 0; i < 500; ++i) {
        float a = rng.range(-2, 2), b = rng.range(-2, 2);
        CHECK(op_ssubtract_quadratic(a, b, 1e-6f) == doctest::Approx(op_subtract(a, b)).epsilon(1e-4));
        CHECK(op_sintersect_quadratic(a, b, 1e-6f) ==
              doctest::Approx(op_intersect(a, b)).epsilon(1e-4));
    }
}

TEST_CASE("material mix: range, sidedness, and distance consistency") {
    clay_test::Lcg rng(45);
    for (int i = 0; i < 2000; ++i) {
        float k = rng.range(0.01f, 0.5f);
        float a = rng.range(-2, 2), b = rng.range(-2, 2);

        cfloat2 q = csmin_quadratic_m(a, b, k);
        CHECK(q.y >= 0.0f);
        CHECK(q.y <= 1.0f);
        CHECK(q.x == doctest::Approx(csmin_quadratic(a, b, k)).epsilon(1e-4));

        cfloat2 c = csmin_cubic_m(a, b, k);
        CHECK(c.y >= 0.0f);
        CHECK(c.y <= 1.0f);

        cfloat2 ch = cchamfer_m(a, b, k);
        CHECK(ch.y >= 0.0f);
        CHECK(ch.y <= 1.0f);
    }
    // far from the blend zone the mix snaps to the winning operand
    CHECK(csmin_quadratic_m(-5.0f, 5.0f, 0.1f).y == doctest::Approx(0.0f));
    CHECK(csmin_quadratic_m(5.0f, -5.0f, 0.1f).y == doctest::Approx(1.0f));
    // equidistant -> even mix
    CHECK(csmin_quadratic_m(0.3f, 0.3f, 0.1f).y == doctest::Approx(0.5f));
}
