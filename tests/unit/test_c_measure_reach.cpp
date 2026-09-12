// Every surface measure is reachable from a host, and every one of them
// actually measures something (issue #511).
//
// WHY A BINDING-LEVEL TEST AND NOT A UNIT ONE. The C++ suite already proves
// what the measures COMPUTE. What was never covered is the only thing that was
// ever wrong: whether a host can call one. The issue was found when a host
// wiring surface measurement discovered five generated measures it could not
// reach from safe code -- surfaced through a feature request, because nothing
// watches for it.
//
// "The engine can do it, the host cannot reach it" is a defect every internal
// test passes through, so the test for it is written against clay.h and
// nothing else. tests/unit/test_c_automask_reach.cpp is the same test one
// surface over, written after CLAY_AUTOMASK_CAVITY and
// CLAY_AUTOMASK_SURFACE_GROUP shipped inert for several releases.
//
// THE GUARD PINS THE LAST ENUMERATOR, and that detail is load-bearing. The
// parity corpus carried `static_assert(cdeform_noise == 13)` for a mid-enum
// value, so it still held while seven deformers went unverified (#535). A
// coverage guard that pins anything but the last is a guard that cannot fire.
//
// AND A MEASURE THAT RETURNS A CONSTANT IS NOT MEASURING. Asserting only
// CLAY_OK would pass on an entry point that returned zeroes for every point
// forever, which is exactly the shape of "generated but unreachable" this issue
// is about. So each case also requires the values to VARY across a fixture
// built to give every measure something to find.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    CDoc() {
        REQUIRE(doc != nullptr);
        REQUIRE(clay_add_sdf_layer(doc, "form", &layer) == CLAY_OK);
    }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

void add_sphere_op(CDoc& d, float r, float x, float y, float z, int32_t op, float k) {
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(item != nullptr);
    clay_item_set_op(item, op);
    REQUIRE(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, k) == CLAY_OK);
    const float pos[3] = {x, y, z};
    REQUIRE(clay_item_set_position(item, pos) == CLAY_OK);
    clay_node_id node = 0;
    const clay_result r2 = clay_layer_add_item(d.doc, d.layer, item, &node);
    clay_item_destroy(item);
    REQUIRE(r2 == CLAY_OK);
}

// TWO BALLS SMOOTH-UNIONED WITH A CRATER CARVED IN ONE.
//
// The caps are convex, the seam between the balls is concave, the normals sweep
// a half turn, and the waist is thin -- which covers curvature, cavity,
// convexity, normal_dir and thickness.
//
// THE CRATER IS THERE FOR OCCLUSION SPECIFICALLY. An earlier version of this
// fixture was two smooth-unioned balls and occlusion read 0 at every point,
// which is not a defect -- a near-convex form occludes nothing, and a measure
// answering 0 everywhere on it is answering correctly. It is a FIXTURE that
// cannot tell a working occlusion measure from an inert one, which is the same
// mistake this file exists to catch, made one level up. A carved pit has walls
// that see each other.
void blended_pair(CDoc& d) {
    add_sphere_op(d, 0.6f, -0.45f, 0, 0, CLAY_OP_ADD, 0.25f);
    add_sphere_op(d, 0.6f, 0.45f, 0, 0, CLAY_OP_ADD, 0.25f);
    add_sphere_op(d, 0.42f, 0.0f, 0.52f, 0.0f, CLAY_OP_SUBTRACT, 0.02f);
}

// POINTS ON THE SURFACE, not near it. clay_measure_points takes its points AS
// GIVEN and does not project them -- the header says so -- and a point off the
// surface makes occlusion and thickness answer about empty space, which reads
// as a constant. The first version of this fixture sampled a ring at the
// surface's rough radius and two measures came back with lo == hi, which is
// the fixture failing the same "is it actually measuring" test the cases apply
// to the engine.
std::vector<float> surface_ring(clay_document* doc, int n) {
    std::vector<float> pts;
    for (int i = 0; i < n; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
        // Fire inward from well outside, around the form, so the hits sweep
        // both caps and both seams.
        const float from[3] = {std::cos(t) * 3.0f, std::sin(t) * 1.2f, std::sin(t * 2.0f) * 1.4f};
        const float dir[3] = {-from[0], -from[1], -from[2]};
        clay_projection proj;
        std::memset(&proj, 0, sizeof proj);
        proj.struct_size = static_cast<uint32_t>(sizeof proj);
        if (clay_project_to_surface(doc, from, dir, 8.0f, &proj) != CLAY_OK) continue;
        if (!proj.hit) continue;
        pts.push_back(proj.position[0]);
        pts.push_back(proj.position[1]);
        pts.push_back(proj.position[2]);
    }
    return pts;
}

const char* measure_name(int m) {
    switch (m) {
        case CLAY_MEASURE_CURVATURE: return "curvature";
        case CLAY_MEASURE_CAVITY: return "cavity";
        case CLAY_MEASURE_CONVEXITY: return "convexity";
        case CLAY_MEASURE_NORMAL_DIR: return "normal_dir";
        case CLAY_MEASURE_OCCLUSION: return "occlusion";
        case CLAY_MEASURE_THICKNESS: return "thickness";
        default: return "?";
    }
}

}  // namespace

TEST_CASE("every surface measure is reachable from C, and none returns a constant") {
    // Pinning the LAST enumerator, so adding a measure is a compile error here
    // rather than a silent gap. See the header: a guard on a mid-enum value is
    // a guard that cannot fire.
    static_assert(CLAY_MEASURE_THICKNESS == 5, "a surface measure was added; widen this test");

    CDoc d;
    blended_pair(d);
    const std::vector<float> pts = surface_ring(d.doc, 96);
    // A fixture that projected nothing would make every case below vacuous.
    REQUIRE(pts.size() >= 3 * 24);
    const std::size_t n = pts.size() / 3;

    clay_measure_params params;
    std::memset(&params, 0, sizeof params);
    params.struct_size = static_cast<uint32_t>(sizeof params);
    REQUIRE(clay_measure_defaults(&params) == CLAY_OK);
    // Small enough to run in a unit test, large enough that occlusion and
    // thickness see the seam rather than nothing.
    params.ray_count = 16;
    params.ray_length = 1.5f;

    for (int m = 0; m <= CLAY_MEASURE_THICKNESS; ++m) {
        CAPTURE(m);
        CAPTURE(measure_name(m));
        std::vector<float> values(n, 0.0f);
        const clay_result r =
            clay_measure_points(d.doc, static_cast<clay_surface_measure>(m), pts.data(), n,
                                &params, values.data(), nullptr);
        REQUIRE(r == CLAY_OK);

        // Every value finite: a NaN reads as "varies" to the check below while
        // being useless to a host.
        for (float v : values) {
            CAPTURE(v);
            REQUIRE(std::isfinite(v));
        }

        // AND IT VARIES. A measure returning one number for every point is
        // reachable and inert, which is the defect this file exists for.
        float lo = values[0], hi = values[0];
        for (float v : values) {
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        CAPTURE(lo);
        CAPTURE(hi);
        CHECK(hi - lo > 1e-4f);
    }
}

TEST_CASE("a measure refuses what it cannot answer, rather than writing garbage") {
    CDoc d;
    blended_pair(d);
    const std::vector<float> pts = surface_ring(d.doc, 12);
    REQUIRE(!pts.empty());
    const std::size_t n = pts.size() / 3;
    clay_measure_params params;
    std::memset(&params, 0, sizeof params);
    params.struct_size = static_cast<uint32_t>(sizeof params);
    REQUIRE(clay_measure_defaults(&params) == CLAY_OK);
    std::vector<float> values(n, 0.0f);

    CHECK(clay_measure_points(nullptr, CLAY_MEASURE_CURVATURE, pts.data(), n, &params,
                              values.data(), nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_measure_points(d.doc, CLAY_MEASURE_CURVATURE, nullptr, n, &params, values.data(),
                              nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_measure_points(d.doc, CLAY_MEASURE_CURVATURE, pts.data(), n, &params, nullptr,
                              nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    // An enumerator past the last one is not a measure.
    CHECK(clay_measure_points(d.doc, static_cast<clay_surface_measure>(CLAY_MEASURE_THICKNESS + 1),
                              pts.data(), n, &params, values.data(),
                              nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}
