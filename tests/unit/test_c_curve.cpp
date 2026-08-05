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
