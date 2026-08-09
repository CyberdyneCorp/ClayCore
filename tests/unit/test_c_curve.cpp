#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/scene/commands.h"
#include "clay/scene/curve.h"
#include "clay/scene/tape.h"

// The C ABI curve surface (c-abi spec). Same standard as the rest: a curve
// built through the boundary and one built on the engine types the way the
// Python bindings do must give the same field, and the tessellation is
// deterministic so "the same" means exactly.

using namespace clay;
using kernel::cf3;

namespace {

const float kSquare[16] = {-1, 0, 0, 0.05f, 0, 1, 0, 0.05f, 1, 0, 0, 0.05f, 0, -1, 0, 0.05f};

std::vector<scene::StrokePoint> engine_square(scene::StrokePointType t) {
    std::vector<scene::StrokePoint> out;
    for (int i = 0; i < 4; ++i) {
        scene::StrokePoint p;
        p.pos = cf3(kSquare[i * 4], kSquare[i * 4 + 1], kSquare[i * 4 + 2]);
        p.radius = kSquare[i * 4 + 3];
        p.type = t;
        out.push_back(p);
    }
    return out;
}

scene::Tape engine_tape(std::vector<scene::StrokePoint> pts, bool closed, float tolerance) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim.type = scene::PrimType::Stroke;
    n.stroke = std::move(pts);
    n.stroke_closed = closed;
    n.curve_tolerance = tolerance;
    l.sdf->insert(std::move(n));
    return scene::compile_document(doc);
}

// The field the C document evaluates at one point, through the ABI.
float eval_c(clay_document* doc, kernel::cfloat3 p) {
    float point[3] = {p.x, p.y, p.z};
    float out = 0.0f;
    REQUIRE(clay_eval_points(doc, "cpu", point, 1, &out, nullptr) == CLAY_OK);
    return out;
}

}  // namespace

TEST_CASE("c curve: a typed chain matches the engine") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);

    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);
    const std::int32_t types[4] = {CLAY_POINT_SPLINE, CLAY_POINT_SPLINE, CLAY_POINT_SPLINE,
                                   CLAY_POINT_SPLINE};
    REQUIRE(clay_item_set_curve_points(item, kSquare, 4, types, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_item_set_curve(item, 0, 0.01f) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    scene::Tape expected = engine_tape(engine_square(scene::StrokePointType::Spline), false, 0.01f);
    for (kernel::cfloat3 p : {cf3(-0.5625f, 0.5625f, 0.0f), cf3(0, 1, 0), cf3(0, 0, 0),
                              cf3(2, 2, 2)})
        CHECK(eval_c(doc, p) == doctest::Approx(expected.eval(p).d));

    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c curve: a NULL type array is the stroke it always was") {
    clay_item* typed = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    clay_item* plain = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(typed != nullptr);
    REQUIRE(plain != nullptr);
    REQUIRE(clay_item_set_curve_points(typed, kSquare, 4, nullptr, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_item_set_stroke_points(plain, kSquare, 4) == CLAY_OK);

    clay_document* a = clay_document_create();
    clay_document* b = clay_document_create();
    clay_layer_id la = 0, lb = 0;
    REQUIRE(clay_add_sdf_layer(a, "l", &la) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(b, "l", &lb) == CLAY_OK);
    REQUIRE(clay_layer_add_item(a, la, typed, nullptr) == CLAY_OK);
    REQUIRE(clay_layer_add_item(b, lb, plain, nullptr) == CLAY_OK);

    for (kernel::cfloat3 p : {cf3(-0.5f, 0.5f, 0.0f), cf3(0, 1, 0), cf3(0.4f, -0.4f, 0.1f)})
        CHECK(eval_c(a, p) == eval_c(b, p));

    clay_item_destroy(typed);
    clay_item_destroy(plain);
    clay_document_destroy(a);
    clay_document_destroy(b);
}

TEST_CASE("c curve: handles and closing reach the engine") {
    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);
    const float two[8] = {-1, 0, 0, 0.1f, 1, 0, 0, 0.1f};
    const std::int32_t types[2] = {CLAY_POINT_BEZIER, CLAY_POINT_BEZIER};
    const float in_handles[6] = {0, 0, 0, 0, 2, 0};
    const float out_handles[6] = {0, 2, 0, 0, 0, 0};
    REQUIRE(clay_item_set_curve_points(item, two, 2, types, in_handles, out_handles) == CLAY_OK);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    // Both handles at +2y put the cubic's peak at y = 1.5.
    CHECK(eval_c(doc, cf3(0, 1.5f, 0)) < 0.0f);
    CHECK(eval_c(doc, cf3(0, 0, 0)) > 0.0f);

    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c curve: editing a placed curve goes through the command vocabulary") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);

    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_stroke_points(item, kSquare, 4) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);

    const kernel::cfloat3 bulge = cf3(-0.5625f, 0.5625f, 0.0f);
    CHECK(eval_c(doc, bulge) > 0.0f);  // a hard chain does not reach it

    const std::int32_t smooth[4] = {CLAY_POINT_SPLINE, CLAY_POINT_SPLINE, CLAY_POINT_SPLINE,
                                    CLAY_POINT_SPLINE};
    REQUIRE(clay_layer_set_stroke_points(doc, layer, node, kSquare, 4, smooth, nullptr, nullptr,
                                         0, 0.01f) == CLAY_OK);
    CHECK(eval_c(doc, bulge) < 0.0f);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(eval_c(doc, bulge) > 0.0f);  // exactly what it was

    SUBCASE("a protected layer refuses it") {
        REQUIRE(clay_document_set_layer_protection(doc, layer, 0, 1) == CLAY_OK);
        CHECK(clay_layer_set_stroke_points(doc, layer, node, kSquare, 4, smooth, nullptr, nullptr,
                                           0, 0.01f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(eval_c(doc, bulge) > 0.0f);
    }

    SUBCASE("an unknown node is a typed error") {
        CHECK(clay_layer_set_stroke_points(doc, layer, 9999, kSquare, 4, smooth, nullptr, nullptr,
                                           0, 0.01f) == CLAY_ERROR_NOT_FOUND);
    }

    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c curve: invalid arguments are rejected") {
    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);

    const std::int32_t bad_types[4] = {0, 0, 9, 0};
    CHECK(clay_item_set_curve_points(item, kSquare, 4, bad_types, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_curve_points(nullptr, kSquare, 4, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_curve_points(item, nullptr, 4, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    CHECK(clay_item_set_curve(item, 0, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_curve(item, 0, -1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_curve(nullptr, 0, 0.01f) == CLAY_ERROR_INVALID_ARGUMENT);

    // Curve settings belong to a stroke item, like its points do.
    const float radius[1] = {1.0f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(sphere != nullptr);
    CHECK(clay_item_set_curve(sphere, 1, 0.01f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_curve_points(sphere, kSquare, 4, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_item_destroy(sphere);
    clay_item_destroy(item);
}

// --- reading a placed curve back (add-curve-points-readback) -----------------

namespace {

const std::int32_t kSpline[4] = {CLAY_POINT_SPLINE, CLAY_POINT_SPLINE, CLAY_POINT_SPLINE,
                                 CLAY_POINT_SPLINE};
const float kIn[12] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f};
const float kOut[12] = {-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f,
                        -0.7f, -0.8f, -0.9f, -1.0f, -1.1f, -1.2f};

// Everything a readback has to answer for, in one place.
struct Readback {
    std::vector<float> xyzr;
    std::vector<std::int32_t> types;
    std::vector<float> in_handles;
    std::vector<float> out_handles;
    std::size_t count = 0;
    std::int32_t closed = -1;
    float tolerance = -1.0f;
};

// The two-call shape a host uses: size query, then fill.
Readback read_curve(clay_document* doc, clay_layer_id layer, clay_node_id node) {
    Readback b;
    REQUIRE(clay_layer_stroke_points(doc, layer, node, nullptr, &b.count, nullptr, nullptr,
                                     nullptr, &b.closed, &b.tolerance) == CLAY_OK);
    b.xyzr.resize(b.count * 4);
    b.types.resize(b.count);
    b.in_handles.resize(b.count * 3);
    b.out_handles.resize(b.count * 3);
    std::size_t capacity = b.count;
    REQUIRE(clay_layer_stroke_points(doc, layer, node, b.xyzr.data(), &capacity, b.types.data(),
                                     b.in_handles.data(), b.out_handles.data(), &b.closed,
                                     &b.tolerance) == CLAY_OK);
    CHECK(capacity == b.count);
    return b;
}

// A placed stroke with the square on it, ready to be read back.
clay_node_id place_square(clay_document* doc, clay_layer_id layer) {
    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_stroke_points(item, kSquare, 4) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);
    return node;
}

}  // namespace

TEST_CASE("c curve: a placed curve reads back exactly what was written") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    clay_node_id node = place_square(doc, layer);

    REQUIRE(clay_layer_set_stroke_points(doc, layer, node, kSquare, 4, kSpline, kIn, kOut, 1,
                                         0.005f) == CLAY_OK);

    Readback b = read_curve(doc, layer, node);
    REQUIRE(b.count == 4);
    CHECK(b.closed == 1);
    CHECK(b.tolerance == 0.005f);
    // Bit-for-bit: the points are stored as authored, so nothing has rounded
    // them and Approx would hide a copy that did.
    for (std::size_t i = 0; i < 16; ++i) CHECK(b.xyzr[i] == kSquare[i]);
    for (std::size_t i = 0; i < 4; ++i) CHECK(b.types[i] == kSpline[i]);
    for (std::size_t i = 0; i < 12; ++i) CHECK(b.in_handles[i] == kIn[i]);
    for (std::size_t i = 0; i < 12; ++i) CHECK(b.out_handles[i] == kOut[i]);

    SUBCASE("and what comes out goes straight back in") {
        const kernel::cfloat3 bulge = cf3(-0.5625f, 0.5625f, 0.0f);
        const float before = eval_c(doc, bulge);
        REQUIRE(clay_layer_set_stroke_points(doc, layer, node, b.xyzr.data(), b.count,
                                             b.types.data(), b.in_handles.data(),
                                             b.out_handles.data(), b.closed,
                                             b.tolerance) == CLAY_OK);
        CHECK(eval_c(doc, bulge) == before);
    }

    SUBCASE("and survives a save and load") {
        const char* path = "c_curve_readback.clayspace";
        REQUIRE(clay_document_save(doc, path) == CLAY_OK);
        clay_document* back = nullptr;
        REQUIRE(clay_document_load(path, &back) == CLAY_OK);
        Readback after = read_curve(back, layer, node);
        REQUIRE(after.count == b.count);
        CHECK(after.closed == b.closed);
        CHECK(after.tolerance == b.tolerance);
        CHECK(after.xyzr == b.xyzr);
        CHECK(after.types == b.types);
        CHECK(after.in_handles == b.in_handles);
        CHECK(after.out_handles == b.out_handles);
        clay_document_destroy(back);
    }

    SUBCASE("the optional arrays are optional, one at a time") {
        std::vector<float> xyzr(16, 0.0f);
        std::size_t capacity = 4;
        REQUIRE(clay_layer_stroke_points(doc, layer, node, xyzr.data(), &capacity, nullptr,
                                         nullptr, nullptr, nullptr, nullptr) == CLAY_OK);
        CHECK(capacity == 4);
        for (std::size_t i = 0; i < 16; ++i) CHECK(xyzr[i] == kSquare[i]);
    }

    clay_document_destroy(doc);
}

TEST_CASE("c curve: the readback is the current state, not the authored one") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
    clay_node_id node = place_square(doc, layer);

    REQUIRE(clay_layer_set_stroke_points(doc, layer, node, kSquare, 4, kSpline, nullptr, nullptr,
                                         0, 0.01f) == CLAY_OK);
    CHECK(read_curve(doc, layer, node).types[0] == CLAY_POINT_SPLINE);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(read_curve(doc, layer, node).types[0] == CLAY_POINT_HARD);

    clay_document_destroy(doc);
}

TEST_CASE("c curve: a profiled tube's guide reads back, and edits") {
    // The case the getter exists for: nothing authored this node in-process,
    // and a profiled tube is a SWEPT node rather than a stroke.
    const float path[12] = {-0.6f, 0.0f, 0.0f, -0.2f, 0.35f, 0.0f,
                            0.2f,  -0.1f, 0.0f, 0.6f, 0.3f,  0.0f};
    clay_tube_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.point_type = CLAY_POINT_SPLINE;
    p.radius_start = p.radius_mid = p.radius_end = 0.1f;
    p.tolerance = 0.01f;
    const float box[2] = {0.09f, 0.05f};
    clay_item* tube = clay_tube_create(path, 4, &p, CLAY_PROFILE_BOX, box, 2);
    REQUIRE(tube != nullptr);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, tube, &node) == CLAY_OK);
    clay_item_destroy(tube);

    Readback b = read_curve(doc, layer, node);
    REQUIRE(b.count == 4);  // control points, not the tessellated guide
    CHECK(b.types[0] == CLAY_POINT_SPLINE);
    CHECK(b.tolerance == doctest::Approx(0.01f));
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(b.xyzr[i * 4 + 0] == path[i * 3 + 0]);
        CHECK(b.xyzr[i * 4 + 1] == path[i * 3 + 1]);
        CHECK(b.xyzr[i * 4 + 2] == path[i * 3 + 2]);
    }

    SUBCASE("and the guide can be replaced in place") {
        const kernel::cfloat3 probe = cf3(-0.2f, 0.35f, 0.0f);
        CHECK(eval_c(doc, probe) < 0.0f);  // inside the tube's bend
        std::vector<float> moved = b.xyzr;
        moved[1 * 4 + 1] = -0.6f;  // drag the bend far away
        REQUIRE(clay_layer_set_stroke_points(doc, layer, node, moved.data(), b.count,
                                             b.types.data(), nullptr, nullptr, 0,
                                             b.tolerance) == CLAY_OK);
        CHECK(eval_c(doc, probe) > 0.0f);
        CHECK(read_curve(doc, layer, node).xyzr[1 * 4 + 1] == -0.6f);
    }

    SUBCASE("but it still cannot be closed") {
        CHECK(clay_layer_set_stroke_points(doc, layer, node, b.xyzr.data(), b.count, nullptr,
                                           nullptr, nullptr, 1, 0.01f) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(read_curve(doc, layer, node).closed == 0);
    }

    SUBCASE("nor cut below the two points a sweep needs") {
        // clay_layer_add_item refuses a one-point guide because the tape gets
        // no swept record at all and the tube silently disappears. Reaching the
        // guide through the placed-node path must not get around that.
        const kernel::cfloat3 probe = cf3(-0.2f, 0.35f, 0.0f);
        for (std::size_t short_count : {std::size_t{0}, std::size_t{1}})
            CHECK(clay_layer_set_stroke_points(doc, layer, node, b.xyzr.data(), short_count,
                                               nullptr, nullptr, nullptr, 0,
                                               b.tolerance) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(read_curve(doc, layer, node).count == 4);
        CHECK(eval_c(doc, probe) < 0.0f);  // still a tube

        // The floor is the SWEEP's, not the point list's: a stroke has none.
        clay_node_id square = place_square(doc, layer);
        CHECK(clay_layer_set_stroke_points(doc, layer, square, kSquare, 1, nullptr, nullptr,
                                           nullptr, 0, 0.01f) == CLAY_OK);
    }

    clay_document_destroy(doc);
}

TEST_CASE("c curve: a short readback buffer reports what it needed") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    clay_node_id node = place_square(doc, layer);

    std::vector<float> xyzr(16, 42.0f);
    std::size_t capacity = 1;
    CHECK(clay_layer_stroke_points(doc, layer, node, xyzr.data(), &capacity, nullptr, nullptr,
                                   nullptr, nullptr, nullptr) == CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(capacity == 4);
    for (float v : xyzr) CHECK(v == 42.0f);  // and it wrote nothing

    clay_document_destroy(doc);
}

TEST_CASE("c curve: reading refuses what it cannot answer") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    clay_node_id node = place_square(doc, layer);
    std::size_t count = 99;

    CHECK(clay_layer_stroke_points(doc, layer, node, nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_stroke_points(nullptr, layer, node, nullptr, &count, nullptr, nullptr,
                                   nullptr, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_stroke_points(doc, 9999, node, nullptr, &count, nullptr, nullptr, nullptr,
                                   nullptr, nullptr) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_stroke_points(doc, layer, 9999, nullptr, &count, nullptr, nullptr, nullptr,
                                   nullptr, nullptr) == CLAY_ERROR_NOT_FOUND);

    SUBCASE("a node that is not a curve is a typed error, not a miss") {
        const float radius[1] = {0.5f};
        clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
        REQUIRE(sphere != nullptr);
        clay_node_id ball = 0;
        REQUIRE(clay_layer_add_item(doc, layer, sphere, &ball) == CLAY_OK);
        clay_item_destroy(sphere);
        CHECK(clay_layer_stroke_points(doc, layer, ball, nullptr, &count, nullptr, nullptr,
                                       nullptr, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a size query takes no point buffers") {
        std::int32_t types[4] = {7, 7, 7, 7};
        CHECK(clay_layer_stroke_points(doc, layer, node, nullptr, &count, types, nullptr, nullptr,
                                       nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(types[0] == 7);
        CHECK(count == 99);  // and it answered nothing
    }

    CHECK(count == 99);  // every failure above left it alone
    clay_document_destroy(doc);
}

TEST_CASE("c curve: a protected layer still reads") {
    // Protection refuses EDITS. Reading a ghosted or locked layer is how a host
    // shows the artist what is on it, so this must not "get fixed" later.
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    clay_node_id node = place_square(doc, layer);

    for (std::int32_t ghost : {0, 1}) {
        REQUIRE(clay_document_set_layer_protection(doc, layer, ghost, 1 - ghost) == CLAY_OK);
        CHECK(read_curve(doc, layer, node).count == 4);
    }
    REQUIRE(clay_document_set_layer_protection(doc, layer, 0, 0) == CLAY_OK);
    REQUIRE(clay_document_set_layer_visible(doc, layer, 0) == CLAY_OK);
    CHECK(read_curve(doc, layer, node).count == 4);

    clay_document_destroy(doc);
}

TEST_CASE("c curve: an empty point list is a count of zero, not an error") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    std::size_t count = 99;
    float xyzr[4] = {0, 0, 0, 0};
    CHECK(clay_layer_stroke_points(doc, layer, node, nullptr, &count, nullptr, nullptr, nullptr,
                                   nullptr, nullptr) == CLAY_OK);
    CHECK(count == 0);
    count = 1;
    CHECK(clay_layer_stroke_points(doc, layer, node, xyzr, &count, nullptr, nullptr, nullptr,
                                   nullptr, nullptr) == CLAY_OK);
    CHECK(count == 0);

    clay_document_destroy(doc);
}
