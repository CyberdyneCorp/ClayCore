#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

// EVERY surface measure, reachable from C and answering (issue #511).
//
// A host wiring surface measurement found that five generated measures could
// not be reached from safe code, and it found that because its field evaluator
// needed occlusion — the gap surfaced through a feature request rather than
// through anything that watches for it.
//
// WHY A BINDING-LEVEL TEST AND NOT A UNIT ONE, the same argument
// test_c_automask_reach.cpp makes one surface over: the C++ suite already
// proves what each measure COMPUTES, and clay.h already DECLARES the entry
// point. Neither of those is the thing that was wrong. "The engine can do it,
// the host cannot reach it" is a defect every internal test passes through, so
// the test for it is written against clay.h and nothing else.
//
// WHAT EACH CASE HAS TO SHOW, and the second half is the one that matters:
//
//   1. The call succeeds for every enumerator, so a measure added to the enum
//      without a path through clay_measure_points fails here rather than in a
//      host six months later.
//   2. The answer VARIES WITH THE SURFACE. A measure that returned CLAY_OK and
//      wrote zeros everywhere would pass (1) and be exactly as unreachable in
//      practice — that is the shape the automask bits had for several releases.
//      So each measure is asked about a shape with the feature it names and a
//      shape without, and the two must differ.
//
// THE ENUM IS ENUMERATED, not listed by hand. A hand-written list is a list
// that goes stale the day someone appends to the enum, which is precisely the
// day this test is supposed to fail.

namespace {

// Every measure clay.h declares, with a name for the failure message. Kept
// beside the enum's own range check below, which is what catches an addition.
struct Measure {
    clay_surface_measure value;
    const char* name;
};

const Measure kMeasures[] = {
    {CLAY_MEASURE_CURVATURE, "CURVATURE"}, {CLAY_MEASURE_CAVITY, "CAVITY"},
    {CLAY_MEASURE_CONVEXITY, "CONVEXITY"}, {CLAY_MEASURE_NORMAL_DIR, "NORMAL_DIR"},
    {CLAY_MEASURE_OCCLUSION, "OCCLUSION"}, {CLAY_MEASURE_THICKNESS, "THICKNESS"},
};

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_node_id add_sphere(Doc& doc, float radius, const float pos[3]) {
    float params[1] = {radius};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_position(item, pos) == CLAY_OK);
    clay_node_id node = 0;
    const clay_result r = clay_layer_add_item(doc.d, doc.layer, item, &node);
    clay_item_destroy(item);
    REQUIRE(r == CLAY_OK);
    return node;
}

clay_measure_params defaults() {
    clay_measure_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    REQUIRE(clay_measure_defaults(&p) == CLAY_OK);
    return p;
}

// Points around the seam of two overlapping spheres — a shape that HAS a
// crevice, a ridge, an occluded pocket and a thin waist, so no measure is being
// asked about a feature the fixture does not contain. A single sphere would
// answer "nothing here" for half of them, correctly, and the test would then be
// asserting that a measure is inert.
std::vector<float> seam_points() {
    std::vector<float> pts;
    for (int i = 0; i < 24; ++i) {
        const float a = 6.2831853f * float(i) / 24.0f;
        pts.push_back(0.0f);                  // on the plane where the two meet
        pts.push_back(0.30f * std::cos(a));
        pts.push_back(0.30f * std::sin(a));
    }
    return pts;
}

}  // namespace

TEST_CASE("every surface measure is reachable from C") {
    // (1) The call itself, for every enumerator. A measure that reaches the
    // enum without reaching this entry point fails here.
    Doc doc;
    const float left[3] = {-0.25f, 0.0f, 0.0f};
    const float right[3] = {0.25f, 0.0f, 0.0f};
    add_sphere(doc, 0.4f, left);
    add_sphere(doc, 0.4f, right);

    const std::vector<float> pts = seam_points();
    const std::size_t count = pts.size() / 3;
    clay_measure_params params = defaults();

    for (const Measure& m : kMeasures) {
        CAPTURE(m.name);
        std::vector<float> values(count, -12345.0f);
        const clay_result r = clay_measure_points(doc.d, m.value, pts.data(), count, &params,
                                                  values.data(), nullptr);
        REQUIRE(r == CLAY_OK);
        // Every slot written — a call that filled half the buffer and returned
        // OK is the "band of garbage" the header warns about.
        for (std::size_t i = 0; i < count; ++i) {
            CAPTURE(i);
            REQUIRE(values[i] != -12345.0f);
            REQUIRE(std::isfinite(values[i]));
        }
    }
}

TEST_CASE("every surface measure answers about the surface, not a constant") {
    // (2) THE HALF THAT MATTERS. A measure that returns CLAY_OK and writes the
    // same number everywhere is as unreachable in practice as one that does not
    // link, and it passes the case above. So each measure is asked about a
    // shape that HAS what it names and one that does not, and the two answers
    // must differ somewhere.
    //
    // VARIES ACROSS POINTS **OR** BETWEEN SHAPES, and the disjunction is not
    // laziness — the six measures discriminate along different axes and a
    // single rule fails an honest one.
    //
    // Measured on this fixture, so the next reader does not have to re-derive
    // which half carries which measure:
    //
    //     measure      between shapes   across points
    //     CURVATURE          0.667          0.000
    //     CAVITY             1.000          0.000
    //     CONVEXITY          0.333          0.000
    //     NORMAL_DIR         0.00014        1.000
    //     OCCLUSION          0.681          0.124
    //     THICKNESS          0.290          0.005
    //
    // NORMAL_DIR carries on the SECOND column alone, and by a full 1.0: it
    // compares a direction, so it sweeps the ring while both shapes present
    // nearly the same normals at any one point. An earlier draft asserted
    // "differs between shapes" only, which put NORMAL_DIR at 0.00014 against a
    // 1e-4 threshold -- passing, by 40%, and certain to flake on another
    // machine. The first three carry on the FIRST column alone, and asserting
    // "varies across points" only would fail them here and fail THICKNESS on a
    // sphere, where a constant answer is the correct one.
    //
    // Every measure clears 1e-3 on at least one column with room to spare.
    //
    // An INERT measure — the failure this case exists for — returns the same
    // number everywhere in both documents, so it fails both halves at once.
    const std::vector<float> pts = seam_points();
    const std::size_t count = pts.size() / 3;
    clay_measure_params params = defaults();

    // Two overlapping spheres: a seam, so a crevice and a ridge and a pocket.
    Doc seam;
    const float left[3] = {-0.25f, 0.0f, 0.0f};
    const float right[3] = {0.25f, 0.0f, 0.0f};
    add_sphere(seam, 0.4f, left);
    add_sphere(seam, 0.4f, right);

    // One big sphere covering the same points: smooth, open, thick.
    Doc smooth;
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    add_sphere(smooth, 0.6f, centre);

    for (const Measure& m : kMeasures) {
        CAPTURE(m.name);
        std::vector<float> a(count, 0.0f), b(count, 0.0f);
        REQUIRE(clay_measure_points(seam.d, m.value, pts.data(), count, &params, a.data(),
                                    nullptr) == CLAY_OK);
        REQUIRE(clay_measure_points(smooth.d, m.value, pts.data(), count, &params, b.data(),
                                    nullptr) == CLAY_OK);

        bool differs_between_shapes = false;
        for (std::size_t i = 0; i < count && !differs_between_shapes; ++i)
            if (std::fabs(a[i] - b[i]) > 1e-3f) differs_between_shapes = true;

        bool varies_across_points = false;
        for (std::size_t i = 1; i < count && !varies_across_points; ++i)
            if (std::fabs(a[i] - a[0]) > 1e-3f) varies_across_points = true;

        INFO("a measure that returns one number everywhere, in both documents, is "
             "reachable but INERT — which is the shape the automask bits had for "
             "several releases and the defect this file exists to catch");
        CHECK((differs_between_shapes || varies_across_points));
    }
}

TEST_CASE("the measure enum has no member this file does not cover") {
    // THE LIST ABOVE GOES STALE THE DAY SOMEONE APPENDS TO THE ENUM, which is
    // exactly the day it should fail. Enumerators are contiguous from 0 here,
    // so the check is that the first value past the table is refused: if a
    // seventh measure is added, clay_measure_points starts accepting it and
    // this fails, pointing at the table.
    //
    // Checked through the ABI rather than against the C enum, because a C test
    // that mentioned a new enumerator would not compile until someone had
    // already edited this file — and then the reminder is redundant.
    Doc doc;
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    add_sphere(doc, 0.4f, centre);
    const std::vector<float> pts = seam_points();
    const std::size_t count = pts.size() / 3;
    clay_measure_params params = defaults();
    std::vector<float> values(count, 0.0f);

    const int past_the_end = static_cast<int>(std::size(kMeasures));
    const clay_result r =
        clay_measure_points(doc.d, static_cast<clay_surface_measure>(past_the_end), pts.data(),
                            count, &params, values.data(), nullptr);
    INFO("clay_measure_points accepted measure " << past_the_end
         << ", so the enum has grown and kMeasures in this file has not");
    CHECK(r != CLAY_OK);
}

TEST_CASE("a measure refuses what it cannot answer rather than inventing it") {
    // The refusals a host actually hits, so a wrapper written against this
    // entry point knows what to propagate.
    Doc doc;
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    add_sphere(doc, 0.4f, centre);
    const std::vector<float> pts = seam_points();
    const std::size_t count = pts.size() / 3;
    clay_measure_params params = defaults();
    std::vector<float> values(count, 0.0f);

    CHECK(clay_measure_points(nullptr, CLAY_MEASURE_CURVATURE, pts.data(), count, &params,
                              values.data(), nullptr) != CLAY_OK);
    CHECK(clay_measure_points(doc.d, CLAY_MEASURE_CURVATURE, nullptr, count, &params,
                              values.data(), nullptr) != CLAY_OK);
    CHECK(clay_measure_points(doc.d, CLAY_MEASURE_CURVATURE, pts.data(), count, &params, nullptr,
                              nullptr) != CLAY_OK);

    // A descriptor that does not declare its size is refused like every other
    // one in this ABI.
    clay_measure_params bad;
    std::memset(&bad, 0, sizeof bad);
    bad.struct_size = 0;
    CHECK(clay_measure_points(doc.d, CLAY_MEASURE_CURVATURE, pts.data(), count, &bad,
                              values.data(), nullptr) != CLAY_OK);
}
