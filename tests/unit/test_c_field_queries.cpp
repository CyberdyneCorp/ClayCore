#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay.h"

// BOUNDED RAYS, CAGE PROJECTION AND SURFACE MEASURES (add-claycore-bridge).
//
// The queries a bake needs and this ABI could not ask for. Every assertion here
// is against an ANALYTIC case, so the right answer is arithmetic rather than a
// rendering — a sphere of radius r has its surface at exactly r.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id sdf = 0;

    explicit Doc(float radius = 0.5f) {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &sdf) == CLAY_OK);
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, sdf, it, &id) == CLAY_OK);
        clay_item_destroy(it);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_projection project(const Doc& doc, const float p[3], const float dir[3], float max_d) {
    clay_projection out{};
    out.struct_size = sizeof(out);
    REQUIRE(clay_project_to_surface(doc.d, p, dir, max_d, &out) == CLAY_OK);
    return out;
}

}  // namespace

// -- bounded rays ------------------------------------------------------------

TEST_CASE("c abi: a surface beyond the bound is not reported") {
    // The distinction the whole entry point exists for: an unbounded raycast
    // makes a miss indistinguishable from a hit on the far side of the model,
    // which is what puts garbage in the seams of a bake.
    Doc doc(0.5f);
    const float origin[3] = {0.0f, 0.0f, 3.0f};
    const float dir[3] = {0.0f, 0.0f, -1.0f};

    // The surface is at z = 0.5, so 2.5 along the ray. Non-degenerate first:
    // the unbounded cast really does find it.
    int32_t hit = 0;
    float t = 0.0f;
    REQUIRE(clay_raycast(doc.d, origin, dir, &hit, &t, nullptr, nullptr) == CLAY_OK);
    REQUIRE(hit != 0);
    REQUIRE(t == doctest::Approx(2.5f).epsilon(0.01));

    // Bounded short of it: a miss.
    REQUIRE(clay_raycast_bounded(doc.d, origin, dir, 0.0f, 2.0f, &hit, &t, nullptr, nullptr) ==
            CLAY_OK);
    CHECK(hit == 0);

    // Bounded past it: the same hit as the unbounded cast.
    REQUIRE(clay_raycast_bounded(doc.d, origin, dir, 0.0f, 3.0f, &hit, &t, nullptr, nullptr) ==
            CLAY_OK);
    CHECK(hit != 0);
    CHECK(t == doctest::Approx(2.5f).epsilon(0.01));
}

TEST_CASE("c abi: a bound that is not a bound is refused") {
    Doc doc;
    const float o[3] = {0, 0, 3}, d[3] = {0, 0, -1};
    int32_t hit = 0;
    CHECK(clay_raycast_bounded(doc.d, o, d, 1.0f, 1.0f, &hit, nullptr, nullptr, nullptr) !=
          CLAY_OK);
}

// -- cage projection ---------------------------------------------------------

TEST_CASE("c abi: a point outside projects inward, with an exact distance") {
    Doc doc(0.5f);
    const float p[3] = {1.5f, 0.0f, 0.0f};
    const float toward[3] = {-1.0f, 0.0f, 0.0f};
    const clay_projection r = project(doc, p, toward, 2.0f);

    CHECK(r.hit != 0);
    // From x = 1.5 travelling -X, the surface is at x = 0.5, so 1.0 away.
    CHECK(r.distance == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(r.position[0] == doctest::Approx(0.5f).epsilon(0.01));
    // Positive: the surface was found ALONG the given direction.
    CHECK(r.distance > 0.0f);
}

TEST_CASE("c abi: a point projects BACKWARD when the surface is behind it") {
    // The case a one-directional implementation silently misses. A cage point
    // from a low-poly mesh sits inside the high-poly wherever the low-poly
    // pinches, and the caller cannot know where that is.
    Doc doc(0.5f);
    const float p[3] = {1.5f, 0.0f, 0.0f};
    // Pointing AWAY from the sphere: nothing ahead, the surface is behind.
    const float away[3] = {1.0f, 0.0f, 0.0f};
    const clay_projection r = project(doc, p, away, 2.0f);

    CHECK(r.hit != 0);
    CHECK(r.distance == doctest::Approx(-1.0f).epsilon(0.01));  // SIGNED, and negative
    CHECK(r.position[0] == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("c abi: a point inside the surface projects outward") {
    Doc doc(0.5f);
    const float inside[3] = {0.1f, 0.0f, 0.0f};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    const clay_projection r = project(doc, inside, dir, 2.0f);
    CHECK(r.hit != 0);
    CHECK(r.position[0] == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("c abi: nothing within the cage is a miss, not a distant hit") {
    Doc doc(0.5f);
    const float far_away[3] = {10.0f, 0.0f, 0.0f};
    const float toward[3] = {-1.0f, 0.0f, 0.0f};
    // The surface is 9.5 away; a 1.0 cage must not find it.
    const clay_projection r = project(doc, far_away, toward, 1.0f);
    CHECK(r.hit == 0);
}

TEST_CASE("c abi: the batch agrees with the single form") {
    Doc doc(0.5f);
    std::vector<float> pts, dirs;
    for (int i = 0; i < 16; ++i) {
        const float a = static_cast<float>(i) * 0.4f;
        pts.push_back(1.2f * std::cos(a));
        pts.push_back(1.2f * std::sin(a));
        pts.push_back(0.0f);
        dirs.push_back(-std::cos(a));
        dirs.push_back(-std::sin(a));
        dirs.push_back(0.0f);
    }
    const std::size_t n = pts.size() / 3;
    std::vector<int32_t> hits(n, 0);
    std::vector<float> dist(n, 0.0f);
    REQUIRE(clay_project_to_surface_many(doc.d, pts.data(), dirs.data(), n, 2.0f, hits.data(),
                                         dist.data(), nullptr, nullptr, nullptr) == CLAY_OK);
    for (std::size_t i = 0; i < n; ++i) {
        const clay_projection one = project(doc, &pts[i * 3], &dirs[i * 3], 2.0f);
        CHECK(hits[i] == one.hit);
        CHECK(dist[i] == doctest::Approx(one.distance).epsilon(0.001));
        // And every one of them found the sphere at radius 0.5 from 1.2 out.
        CHECK(dist[i] == doctest::Approx(0.7f).epsilon(0.02));
    }
}

// -- surface measures --------------------------------------------------------

TEST_CASE("c abi: measures come back, and a sphere is convex") {
    Doc doc(0.5f);
    clay_measure_params p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_measure_defaults(&p) == CLAY_OK);
    p.scale = 0.5f;

    const float pts[6] = {0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f};
    float convex[2] = {-1.0f, -1.0f};
    float cavity[2] = {-1.0f, -1.0f};
    REQUIRE(clay_measure_points(doc.d, CLAY_MEASURE_CONVEXITY, pts, 2, &p, convex, nullptr) ==
            CLAY_OK);
    REQUIRE(clay_measure_points(doc.d, CLAY_MEASURE_CAVITY, pts, 2, &p, cavity, nullptr) ==
            CLAY_OK);
    for (int i = 0; i < 2; ++i) {
        CHECK(convex[i] > 0.5f);
        CHECK(cavity[i] == 0.0f);
    }
}

TEST_CASE("c abi: occlusion is reproducible with the same seed") {
    Doc doc(0.5f);
    clay_measure_params p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_measure_defaults(&p) == CLAY_OK);
    p.ray_length = 0.5f;
    p.ray_count = 16;
    p.seed = 4242;

    const float pt[3] = {0.5f, 0.0f, 0.0f};
    float a = -1.0f, b = -1.0f;
    REQUIRE(clay_measure_points(doc.d, CLAY_MEASURE_OCCLUSION, pt, 1, &p, &a, nullptr) == CLAY_OK);
    REQUIRE(clay_measure_points(doc.d, CLAY_MEASURE_OCCLUSION, pt, 1, &p, &b, nullptr) == CLAY_OK);
    CHECK(a == b);  // exactly
}

TEST_CASE("c abi: null params means the defaults, not an error") {
    // A caller measuring curvature with no opinion about the stencil should not
    // have to fill in a struct to say so.
    Doc doc(0.5f);
    const float pt[3] = {0.5f, 0.0f, 0.0f};
    float v = -1.0f;
    CHECK(clay_measure_points(doc.d, CLAY_MEASURE_CURVATURE, pt, 1, nullptr, &v, nullptr) ==
          CLAY_OK);
    CHECK(v >= 0.0f);
}

TEST_CASE("c abi: a procedural mask is reachable at last") {
    // brush::mask_from_surface existed with tests and was reachable from no
    // host at all — the same gap add-surface-groups had. This is the
    // regression for it.
    Doc doc(0.5f);
    clay_measure_params p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_measure_defaults(&p) == CLAY_OK);
    p.scale = 0.5f;

    const float lo[3] = {-0.7f, -0.7f, -0.7f};
    const float hi[3] = {0.7f, 0.7f, 0.7f};
    clay_mask* mask = nullptr;
    REQUIRE(clay_mask_from_surface(doc.d, CLAY_MEASURE_CONVEXITY, lo, hi, 0.03f, 0.0f, &p, &mask,
                                   nullptr) == CLAY_OK);
    REQUIRE(mask != nullptr);
    size_t painted = 0;
    REQUIRE(clay_mask_painted_count(mask, &painted) == CLAY_OK);
    CHECK(painted > 100);  // a convex sphere really does paint

    // And it is an ordinary mask: usable everywhere a painted one is.
    float value = -1.0f;
    const float on[3] = {0.5f, 0.0f, 0.0f};
    REQUIRE(clay_mask_sample(mask, on, &value) == CLAY_OK);
    CHECK(value > 0.0f);
    clay_mask_destroy(mask);
}

TEST_CASE("c abi: an unbounded region is refused rather than walked forever") {
    Doc doc(0.5f);
    // INVERTED, which is what Aabb::empty() means: min > max. A zero-VOLUME
    // box is not empty and legitimately yields a one-cell mask, so testing that
    // instead would have asserted behaviour nobody specified.
    const float lo[3] = {1.0f, 1.0f, 1.0f};
    const float hi[3] = {-1.0f, -1.0f, -1.0f};
    clay_mask* mask = nullptr;
    CHECK(clay_mask_from_surface(doc.d, CLAY_MEASURE_CURVATURE, lo, hi, 0.05f, 0.0f, nullptr,
                                 &mask, nullptr) != CLAY_OK);
    CHECK(mask == nullptr);
}
