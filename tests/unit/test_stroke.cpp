#include <doctest/doctest.h>

#include <vector>

#include "clay/kernel/prim3d.h"
#include "clay/kernel/stroke.h"
#include "kernel_utils.h"

using namespace clay::kernel;

TEST_CASE("single-point stroke is a sphere") {
    std::vector<CStrokePoint> pts = {{cf3(1, 0, 0), 0.5f}};
    int seg;
    float t;
    clay_test::Lcg rng(81);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        CHECK(sd_stroke(pts.data(), 1, p, 0.0f, &seg, &t) ==
              doctest::Approx(sd_sphere(p - cf3(1, 0, 0), 0.5f)).epsilon(1e-5));
    }
}

TEST_CASE("equal-radius two-point stroke is a capsule") {
    std::vector<CStrokePoint> pts = {{cf3(0, 0, 0), 0.3f}, {cf3(1, 0, 0), 0.3f}};
    int seg;
    float t;
    clay_test::Lcg rng(82);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        CHECK(sd_stroke(pts.data(), 2, p, 0.0f, &seg, &t) ==
              doctest::Approx(sd_capsule(p, cf3(0, 0, 0), cf3(1, 0, 0), 0.3f)).epsilon(1e-5));
    }
}

TEST_CASE("varying-radius stroke: chain of round cones, single item") {
    std::vector<CStrokePoint> pts = {{cf3(0, 0, 0), 0.4f},
                                     {cf3(1, 0, 0), 0.3f},
                                     {cf3(2, 0.5f, 0), 0.2f},
                                     {cf3(3, 0.5f, 0), 0.1f}};
    int seg;
    float t;
    auto brute = [&](cfloat3 p) {
        float best = 3.4e38f;
        for (int i = 0; i + 1 < static_cast<int>(pts.size()); ++i) {
            best = cmin(best, sd_round_cone_ab(p, pts[static_cast<std::size_t>(i)].pos,
                                               pts[static_cast<std::size_t>(i) + 1].pos,
                                               pts[static_cast<std::size_t>(i)].radius,
                                               pts[static_cast<std::size_t>(i) + 1].radius));
        }
        return best;
    };
    clay_test::Lcg rng(83);
    for (int i = 0; i < 300; ++i) {
        cfloat3 p = rng.vec3(-1, 4);
        CHECK(sd_stroke(pts.data(), 4, p, 0.0f, &seg, &t) ==
              doctest::Approx(brute(p)).epsilon(1e-5));
    }
}

TEST_CASE("stroke attribution: closest segment and parameter") {
    std::vector<CStrokePoint> pts = {{cf3(0, 0, 0), 0.2f},
                                     {cf3(2, 0, 0), 0.2f},
                                     {cf3(2, 2, 0), 0.2f}};
    int seg;
    float t;
    // near the middle of the first segment
    sd_stroke(pts.data(), 3, cf3(1, 0.5f, 0), 0.0f, &seg, &t);
    CHECK(seg == 0);
    CHECK(t == doctest::Approx(0.5f).epsilon(1e-3));
    // near the second segment
    sd_stroke(pts.data(), 3, cf3(2.5f, 1.5f, 0), 0.0f, &seg, &t);
    CHECK(seg == 1);
    CHECK(t == doctest::Approx(0.75f).epsilon(1e-3));
}

TEST_CASE("smooth stroke is a conservative field and adds material") {
    std::vector<CStrokePoint> pts = {{cf3(-1, 0, 0), 0.3f},
                                     {cf3(0, 0.6f, 0), 0.25f},
                                     {cf3(1, 0, 0), 0.3f}};
    int seg;
    float t;
    auto smooth = [&](cfloat3 p) { return sd_stroke(pts.data(), 3, p, 0.1f, &seg, &t); };
    auto hard = [&](cfloat3 p) { return sd_stroke(pts.data(), 3, p, 0.0f, &seg, &t); };
    clay_test::check_conservative_steps(smooth, 1.0f, 2.5f, 400, 84);
    clay_test::Lcg rng(85);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-2, 2);
        CHECK(smooth(p) <= hard(p) + 1e-5f);  // blending only adds material
    }
}
