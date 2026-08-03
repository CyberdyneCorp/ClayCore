#include <doctest/doctest.h>

#include "clay/math/geom.h"
#include "clay/math/transform.h"
#include "kernel_utils.h"

using namespace clay;
using namespace clay::kernel;
using math::Aabb;
using math::Quat;
using math::Transform;

TEST_CASE("quat rotation matches known angles") {
    Quat q = Quat::from_axis_angle(cf3(0, 0, 1), 1.5707963f);  // 90 deg around Z
    cfloat3 v = q.rotate(cf3(1, 0, 0));
    CHECK(v.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(v.y == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("transform apply/inverse round trip") {
    clay_test::Lcg rng(201);
    for (int i = 0; i < 200; ++i) {
        Transform t;
        t.position = rng.vec3(-5, 5);
        t.rotation = Quat::from_axis_angle(rng.unit3(), rng.range(-3, 3));
        t.scale = rng.range(0.3f, 3.0f);
        cfloat3 p = rng.vec3(-5, 5);
        cfloat3 q = t.apply_inverse(t.apply(p));
        CHECK(clength(q - p) < 1e-4f);
        // matrix path agrees with quaternion path
        cfloat3 m = cmul_point(t.matrix(), p);
        CHECK(clength(m - t.apply(p)) < 1e-4f);
        cfloat3 mi = cmul_point(t.inverse_matrix(), t.apply(p));
        CHECK(clength(mi - p) < 1e-4f);
    }
}

TEST_CASE("transform composition") {
    clay_test::Lcg rng(202);
    for (int i = 0; i < 100; ++i) {
        Transform a, b;
        a.position = rng.vec3(-2, 2);
        a.rotation = Quat::from_axis_angle(rng.unit3(), rng.range(-3, 3));
        a.scale = rng.range(0.5f, 2.0f);
        b.position = rng.vec3(-2, 2);
        b.rotation = Quat::from_axis_angle(rng.unit3(), rng.range(-3, 3));
        b.scale = rng.range(0.5f, 2.0f);
        cfloat3 p = rng.vec3(-2, 2);
        cfloat3 lhs = (a * b).apply(p);
        cfloat3 rhs = a.apply(b.apply(p));
        CHECK(clength(lhs - rhs) < 1e-3f);
        // matrix product agrees
        cfloat3 mm = cmul_point(math::mul(a.matrix(), b.matrix()), p);
        CHECK(clength(mm - rhs) < 1e-3f);
    }
}

TEST_CASE("aabb: expand, dilate, intersects, distance, transform") {
    Aabb b;
    CHECK(b.empty());
    b.expand(cf3(-1, -1, -1));
    b.expand(cf3(1, 2, 3));
    CHECK(!b.empty());
    CHECK(b.contains(cf3(0, 0, 0)));
    CHECK(!b.contains(cf3(0, 3, 0)));
    CHECK(b.distance(cf3(3, 0, 0)) == doctest::Approx(2.0f));
    CHECK(b.distance(cf3(0, 0, 0)) == doctest::Approx(0.0f));

    Aabb d = b.dilated(0.5f);
    CHECK(d.min.x == doctest::Approx(-1.5f));
    CHECK(d.max.y == doctest::Approx(2.5f));

    Aabb far_box{cf3(10, 10, 10), cf3(11, 11, 11)};
    CHECK(!b.intersects(far_box));
    CHECK(b.intersects(d));

    // rotated unit box bound contains the rotated corners
    Transform t;
    t.rotation = Quat::from_axis_angle(cf3(0, 0, 1), 0.7853982f);  // 45 deg
    Aabb unit{cf3(-1, -1, -1), cf3(1, 1, 1)};
    Aabb r = unit.transformed(t.matrix());
    CHECK(r.max.x == doctest::Approx(csqrt(2.0f)).epsilon(1e-4));
    CHECK(r.max.z == doctest::Approx(1.0f).epsilon(1e-4));

    CHECK(Aabb::infinite().is_infinite());
    CHECK(Aabb::infinite().intersects(unit));
}

TEST_CASE("ray-aabb slab test") {
    Aabb b{cf3(-1, -1, -1), cf3(1, 1, 1)};
    float t0, t1;
    REQUIRE(math::ray_aabb({cf3(0, 0, -5), cf3(0, 0, 1)}, b, &t0, &t1));
    CHECK(t0 == doctest::Approx(4.0f));
    CHECK(t1 == doctest::Approx(6.0f));
    CHECK(!math::ray_aabb({cf3(0, 5, -5), cf3(0, 0, 1)}, b, &t0, &t1));
    // axis-parallel ray inside the slab
    REQUIRE(math::ray_aabb({cf3(0.5f, 0.5f, -5), cf3(0, 0, 1)}, b, &t0, &t1));
}

TEST_CASE("frustum-aabb") {
    // axis-aligned box frustum: |x|,|y|,|z| <= 2
    math::Frustum f{{{cf3(1, 0, 0), 2},
                     {cf3(-1, 0, 0), 2},
                     {cf3(0, 1, 0), 2},
                     {cf3(0, -1, 0), 2},
                     {cf3(0, 0, 1), 2},
                     {cf3(0, 0, -1), 2}}};
    CHECK(f.intersects(Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)}));
    CHECK(f.intersects(Aabb{cf3(1, 1, 1), cf3(3, 3, 3)}));  // partial overlap
    CHECK(!f.intersects(Aabb{cf3(3, 3, 3), cf3(4, 4, 4)}));
}
