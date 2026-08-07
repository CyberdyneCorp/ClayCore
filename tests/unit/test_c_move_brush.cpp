#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "clay.h"

// The Move brush across the C ABI (c-abi spec, add-move-brush).

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

clay_move_params move_params(float radius) {
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.radius = radius;
    return p;
}

// Two balls smooth-unioned: the case a per-item grab gets wrong.
clay_layer_id blended_form(clay_document* doc) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "form", &layer) == CLAY_OK);
    for (float x : {-0.45f, 0.45f}) {
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_SPHERE;
        d.params[0] = 0.5f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.25f;
        d.position[0] = x;
        clay_node_id node = 0;
        REQUIRE(clay_add_item(doc, layer, &d, &node) == CLAY_OK);
    }
    return layer;
}

float top_at(clay_document* doc, float x) {
    float last = 1.0f;
    for (float y = 1.6f; y > -1.6f; y -= 0.002f) {
        const float point[3] = {x, y, 0.0f};
        float d = 0.0f;
        REQUIRE(clay_eval_points(doc, nullptr, point, 1, &d, nullptr) == CLAY_OK);
        if (d <= 0.0f && last > 0.0f) return y;
        last = d;
    }
    return 0.0f;
}

}  // namespace

TEST_CASE("c move: a drag moves the assembled surface, symmetrically") {
    CDoc base;
    const clay_layer_id base_layer = blended_form(base.doc);
    (void)base_layer;
    const float before_left = top_at(base.doc, -0.45f);
    const float before_centre = top_at(base.doc, 0.0f);

    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};
    const clay_move_params p = move_params(0.8f);
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(c.doc, layer, centre, displacement, &p, &applied) ==
            CLAY_OK);
    CHECK(applied == 2);  // both items took a share

    const float left = top_at(c.doc, -0.45f) - before_left;
    const float right = top_at(c.doc, 0.45f) - before_left;  // symmetric form
    const float middle = top_at(c.doc, 0.0f) - before_centre;
    CHECK(left > 0.0f);
    CHECK(left == doctest::Approx(right).epsilon(0.1));
    CHECK(middle >= left);
    CHECK(middle < 0.4f);  // grab pulls short of the displacement, by design
}

TEST_CASE("c move: the whole drag is one undo step") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    REQUIRE(clay_document_enable_undo(c.doc) == CLAY_OK);
    const float before = top_at(c.doc, 0.0f);

    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};
    const clay_move_params p = move_params(0.8f);
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(c.doc, layer, centre, displacement, &p, &applied) ==
            CLAY_OK);
    REQUIRE(applied == 2);
    CHECK(top_at(c.doc, 0.0f) > before);

    int32_t enabled = 0;
    size_t depth = 0, redo = 0;
    REQUIRE(clay_document_undo_state(c.doc, &enabled, &depth, &redo) == CLAY_OK);
    CHECK(depth == 1);  // two items, one gesture, one step

    int32_t undone = 0;
    REQUIRE(clay_document_undo(c.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(top_at(c.doc, 0.0f) == doctest::Approx(before));
}

TEST_CASE("c move: a drag that reaches nothing succeeds and changes nothing") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float before = top_at(c.doc, 0.0f);
    const float far[3] = {40.0f, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};
    const clay_move_params p = move_params(0.8f);
    size_t applied = 99;
    CHECK(clay_layer_move_surface(c.doc, layer, far, displacement, &p, &applied) == CLAY_OK);
    CHECK(applied == 0);
    CHECK(top_at(c.doc, 0.0f) == doctest::Approx(before));
}

TEST_CASE("c move: bad arguments are refused") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};

    clay_move_params bad = move_params(0.0f);
    CHECK(clay_layer_move_surface(c.doc, layer, centre, displacement, &bad, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    bad = move_params(0.8f);
    bad.struct_size = 4;  // below the original layout
    CHECK(clay_layer_move_surface(c.doc, layer, centre, displacement, &bad, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    const clay_move_params good = move_params(0.8f);
    CHECK(clay_layer_move_surface(c.doc, 999, centre, displacement, &good, nullptr) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_move_surface(c.doc, layer, nullptr, displacement, &good, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_move_surface(c.doc, layer, centre, displacement, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c move: a deformer can be added to a node already in a document") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float before = top_at(c.doc, -0.45f);

    // The mutation nothing could do before: clay_item_add_deformer builds an
    // item, this edits a placed one.
    // centre(3), radius, displacement(3), front_only — eight, per kDeformParams.
    const float params[8] = {0.0f, 0.0f, 0.0f, 0.8f, 0.0f, 0.4f, 0.0f, 0.0f};
    REQUIRE(clay_layer_add_deformer(c.doc, layer, 1, CLAY_DEFORM_GRAB, params, 8, 0, 1) ==
            CLAY_OK);
    CHECK(top_at(c.doc, -0.45f) > before);

    CHECK(clay_layer_add_deformer(c.doc, layer, 9999, CLAY_DEFORM_GRAB, params, 8, 0, 1) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_add_deformer(c.doc, layer, 1, 999, params, 8, 0, 1) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c move: magnify and noise are reachable from C at all") {
    // Regression. The deformer bound check stopped at POSE_LINE, so the two
    // kinds added after it were declared, documented, given parameter counts
    // and handled by the decoder — and refused at the door. Python could reach
    // them; C could not, and the parity gate checks enumerators rather than
    // calls, so nothing noticed.
    const float radius[1] = {0.5f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(item != nullptr);
    const float magnify[5] = {0.0f, 0.0f, 0.0f, 0.4f, 0.5f};
    CHECK(clay_item_add_deformer(item, CLAY_DEFORM_MAGNIFY, magnify, 5, 0) == CLAY_OK);
    const float noise[5] = {0.15f, 2.0f, 3.0f, 0.5f, 7.0f};
    CHECK(clay_item_add_deformer(item, CLAY_DEFORM_NOISE, noise, 5, 0) == CLAY_OK);
    // ...and one past the end is still refused.
    CHECK(clay_item_add_deformer(item, CLAY_DEFORM_NOISE + 1, noise, 5, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(item);
}
