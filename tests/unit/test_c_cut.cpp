#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/cut/cut.h"
#include "clay/scene/tape.h"

// The C ABI cut surface (c-abi spec: the cut tool). Same standard as the rest:
// a cut resolved through the boundary and one resolved on the engine types the
// way the Python bindings do must give the same field.

using namespace clay;
using kernel::cf3;

namespace {

clay_cut_desc front_cut(std::int32_t shape = CLAY_CUT_RECT) {
    clay_cut_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<std::uint32_t>(sizeof d);
    d.origin[2] = -4.0f;
    d.right[0] = 1.0f;
    d.up[1] = 1.0f;
    d.forward[2] = 1.0f;
    d.shape = shape;
    d.half_width = 0.4f;
    d.half_height = 0.4f;
    d.radius = 0.5f;
    d.region_min[0] = d.region_min[1] = d.region_min[2] = -1.0f;
    d.region_max[0] = d.region_max[1] = d.region_max[2] = 1.0f;
    return d;
}

// A 2x2x2 block with the given item subtracted, as a C document.
clay_document* block_cut_by(clay_item* cutter, std::int32_t op) {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    const float box[3] = {1.0f, 1.0f, 1.0f};
    clay_item* solid = clay_item_create(CLAY_PRIM_BOX, box, 3);
    REQUIRE(solid != nullptr);
    REQUIRE(clay_layer_add_item(doc, layer, solid, nullptr) == CLAY_OK);
    clay_item_destroy(solid);
    if (cutter) {
        REQUIRE(clay_item_set_op(cutter, op) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, cutter, nullptr) == CLAY_OK);
    }
    return doc;
}

float eval_c(clay_document* doc, kernel::cfloat3 p) {
    float point[3] = {p.x, p.y, p.z};
    float out = 0.0f;
    REQUIRE(clay_eval_points(doc, "cpu", point, 1, &out, nullptr) == CLAY_OK);
    return out;
}

}  // namespace

TEST_CASE("c cut: a rectangle cuts through, and matches the engine") {
    clay_cut_desc d = front_cut();
    clay_item* cutter = clay_cut_create(&d, nullptr, 0);
    REQUIRE(cutter != nullptr);
    clay_document* doc = block_cut_by(cutter, CLAY_OP_SUBTRACT);

    for (float z : {-0.9f, 0.0f, 0.9f}) CHECK(eval_c(doc, cf3(0, 0, z)) > 0.0f);
    CHECK(eval_c(doc, cf3(0.8f, 0.8f, 0.0f)) < 0.0f);

    // The same cut on the engine types: the boundary must not shift it.
    cut::CutFrame frame;
    frame.origin = cf3(0, 0, -4);
    frame.right = cf3(1, 0, 0);
    frame.up = cf3(0, 1, 0);
    frame.forward = cf3(0, 0, 1);
    auto expected = cut::cut_item(frame, cut::CutShape::rect(0.4f, 0.4f),
                                  math::Aabb(cf3(-1, -1, -1), cf3(1, 1, 1)));
    REQUIRE(expected.has_value());
    scene::Document ref;
    scene::Layer& l = ref.add_sdf_layer("l");
    scene::Node solid;
    solid.prim = scene::Prim::box(cf3(1, 1, 1));
    l.sdf->insert(std::move(solid));
    scene::Node placed = *expected;
    placed.op = scene::Op::Subtract;
    l.sdf->insert(std::move(placed));
    scene::Tape tape = scene::compile_document(ref);

    for (kernel::cfloat3 p : {cf3(0, 0, 0), cf3(0.45f, 0, 0), cf3(0.35f, 0, 0), cf3(0.9f, 0.9f, 0)})
        CHECK(eval_c(doc, p) == doctest::Approx(tape.eval(p).d));

    clay_item_destroy(cutter);
    clay_document_destroy(doc);
}

TEST_CASE("c cut: circle, polygon and a spline lasso") {
    SUBCASE("circle") {
        clay_cut_desc d = front_cut(CLAY_CUT_CIRCLE);
        clay_item* cutter = clay_cut_create(&d, nullptr, 0);
        REQUIRE(cutter != nullptr);
        clay_document* doc = block_cut_by(cutter, CLAY_OP_SUBTRACT);
        CHECK(eval_c(doc, cf3(0, 0, 0)) > 0.0f);
        CHECK(eval_c(doc, cf3(0.45f, 0.45f, 0.0f)) < 0.0f);  // outside the circle
        clay_item_destroy(cutter);
        clay_document_destroy(doc);
    }
    SUBCASE("polygon") {
        clay_cut_desc d = front_cut(CLAY_CUT_POLYGON);
        const float tri[6] = {-0.6f, -0.4f, 0.6f, -0.4f, 0.0f, 0.6f};
        clay_item* cutter = clay_cut_create(&d, tri, 3);
        REQUIRE(cutter != nullptr);
        clay_document* doc = block_cut_by(cutter, CLAY_OP_SUBTRACT);
        CHECK(eval_c(doc, cf3(0.0f, -0.2f, 0.0f)) > 0.0f);
        CHECK(eval_c(doc, cf3(0.55f, 0.5f, 0.0f)) < 0.0f);
        clay_item_destroy(cutter);
        clay_document_destroy(doc);
    }
    SUBCASE("a spline lasso flattens through the curve tessellator") {
        const float control[16] = {-0.5f, 0, 0, 0, 0, 0.5f, 0, 0,
                                   0.5f,  0, 0, 0, 0, -0.5f, 0, 0};
        const std::int32_t types[4] = {CLAY_POINT_SPLINE, CLAY_POINT_SPLINE, CLAY_POINT_SPLINE,
                                       CLAY_POINT_SPLINE};
        std::size_t count = 0;
        REQUIRE(clay_cut_polygon_from_curve(control, 4, types, 0.005f, nullptr, &count) ==
                CLAY_OK);
        CHECK(count > 4);  // it really tessellated

        std::vector<float> outline(count * 2);
        std::size_t capacity = count;
        REQUIRE(clay_cut_polygon_from_curve(control, 4, types, 0.005f, outline.data(),
                                            &capacity) == CLAY_OK);
        CHECK(capacity == count);

        clay_cut_desc d = front_cut(CLAY_CUT_POLYGON);
        clay_item* cutter = clay_cut_create(&d, outline.data(), count);
        REQUIRE(cutter != nullptr);
        clay_document* doc = block_cut_by(cutter, CLAY_OP_SUBTRACT);
        // Between the control polygon's chord and the spline's bulge.
        CHECK(eval_c(doc, cf3(-0.28f, 0.28f, 0.0f)) > 0.0f);
        clay_item_destroy(cutter);
        clay_document_destroy(doc);

        SUBCASE("a short buffer reports what it needed") {
            std::vector<float> small(2);
            std::size_t one = 1;
            CHECK(clay_cut_polygon_from_curve(control, 4, types, 0.005f, small.data(), &one) ==
                  CLAY_ERROR_BUFFER_TOO_SMALL);
            CHECK(one == count);
        }
    }
}

TEST_CASE("c cut: keep-inner and keep-outer are the op") {
    clay_cut_desc d = front_cut(CLAY_CUT_CIRCLE);
    clay_item* a = clay_cut_create(&d, nullptr, 0);
    clay_item* b = clay_cut_create(&d, nullptr, 0);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    clay_document* removed = block_cut_by(a, CLAY_OP_SUBTRACT);
    clay_document* kept = block_cut_by(b, CLAY_OP_INTERSECT);

    for (float x = -0.9f; x <= 0.9f; x += 0.23f) {
        kernel::cfloat3 p = cf3(x, 0.1f, 0.0f);
        CHECK((eval_c(removed, p) < 0.0f) != (eval_c(kept, p) < 0.0f));
    }

    clay_item_destroy(a);
    clay_item_destroy(b);
    clay_document_destroy(removed);
    clay_document_destroy(kept);
}

TEST_CASE("c cut: an explicit extent cuts only that far") {
    clay_cut_desc d = front_cut();
    d.origin[2] = -8.0f;
    d.near_extent = 0.0f;
    d.far_extent = 8.0f;  // reaches z = 0 and no further
    clay_item* cutter = clay_cut_create(&d, nullptr, 0);
    REQUIRE(cutter != nullptr);
    clay_document* doc = block_cut_by(cutter, CLAY_OP_SUBTRACT);
    CHECK(eval_c(doc, cf3(0, 0, -0.5f)) > 0.0f);
    CHECK(eval_c(doc, cf3(0, 0, 0.5f)) < 0.0f);
    clay_item_destroy(cutter);
    clay_document_destroy(doc);
}

TEST_CASE("c cut: invalid descriptors are refused") {
    CHECK(clay_cut_create(nullptr, nullptr, 0) == nullptr);

    clay_cut_desc short_desc = front_cut();
    short_desc.struct_size = 4;
    CHECK(clay_cut_create(&short_desc, nullptr, 0) == nullptr);

    clay_cut_desc skewed = front_cut();
    skewed.up[0] = 0.5f;  // no longer orthogonal to right
    CHECK(clay_cut_create(&skewed, nullptr, 0) == nullptr);

    clay_cut_desc unnormalized = front_cut();
    unnormalized.right[0] = 2.0f;
    CHECK(clay_cut_create(&unnormalized, nullptr, 0) == nullptr);

    clay_cut_desc flat = front_cut();
    flat.half_width = 0.0f;
    CHECK(clay_cut_create(&flat, nullptr, 0) == nullptr);

    clay_cut_desc unknown = front_cut(77);
    CHECK(clay_cut_create(&unknown, nullptr, 0) == nullptr);

    clay_cut_desc negative = front_cut();
    negative.rounding = -1.0f;
    CHECK(clay_cut_create(&negative, nullptr, 0) == nullptr);

    clay_cut_desc poly = front_cut(CLAY_CUT_POLYGON);
    CHECK(clay_cut_create(&poly, nullptr, 3) == nullptr);   // null outline
    const float two[4] = {0, 0, 1, 1};
    CHECK(clay_cut_create(&poly, two, 2) == nullptr);       // not an area

    std::size_t count = 0;
    const float control[8] = {0, 0, 0, 0, 1, 0, 0, 0};
    CHECK(clay_cut_polygon_from_curve(control, 2, nullptr, 0.0f, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_cut_polygon_from_curve(control, 2, nullptr, 0.01f, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c cut: an open trim curve closes against the frame bounds") {
    // ZBrush's Trim Curve. The size-query pattern first, as the lasso uses.
    std::vector<float> pts;
    for (int i = 0; i < 7; ++i) {
        const float x = -1.2f + 0.4f * static_cast<float>(i);
        pts.push_back(x);
        pts.push_back(0.2f * std::sin(x * 2.6f));
        pts.push_back(0.0f);
        pts.push_back(0.0f);
    }
    const float extent[2] = {3.0f, 3.0f};

    std::size_t n = 0;
    REQUIRE(clay_cut_polygon_from_open_curve(pts.data(), 7, nullptr, CLAY_TRIM_BELOW, extent,
                                             0.01f, nullptr, &n) == CLAY_OK);
    REQUIRE(n > 7);  // the stroke, plus the two vertices that close it

    std::vector<float> xy(n * 2);
    REQUIRE(clay_cut_polygon_from_open_curve(pts.data(), 7, nullptr, CLAY_TRIM_BELOW, extent,
                                             0.01f, xy.data(), &n) == CLAY_OK);
    // The closing edge runs along the bound on the side being covered.
    CHECK(xy[(n - 1) * 2 + 1] == doctest::Approx(-3.0f));
    CHECK(xy[(n - 2) * 2 + 1] == doctest::Approx(-3.0f));

    SUBCASE("the other side closes the other way") {
        std::size_t m = 0;
        REQUIRE(clay_cut_polygon_from_open_curve(pts.data(), 7, nullptr, CLAY_TRIM_ABOVE,
                                                 extent, 0.01f, nullptr, &m) == CLAY_OK);
        std::vector<float> up(m * 2);
        REQUIRE(clay_cut_polygon_from_open_curve(pts.data(), 7, nullptr, CLAY_TRIM_ABOVE,
                                                 extent, 0.01f, up.data(), &m) == CLAY_OK);
        CHECK(up[(m - 1) * 2 + 1] == doctest::Approx(3.0f));
    }

    SUBCASE("and it refuses what is not a stroke") {
        std::size_t m = 0;
        CHECK(clay_cut_polygon_from_open_curve(pts.data(), 1, nullptr, CLAY_TRIM_BELOW, extent,
                                               0.01f, nullptr, &m) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_cut_polygon_from_open_curve(pts.data(), 7, nullptr, 99, extent, 0.01f,
                                               nullptr, &m) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_cut_polygon_from_open_curve(pts.data(), 7, nullptr, CLAY_TRIM_BELOW, extent,
                                               0.0f, nullptr, &m) == CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("c abi: a tube resolves from a path") {
    // Nomad's Tubes across the boundary. The round tube must stay EXACT, which
    // is the property that makes it the cheap one to reach for.
    const float path[12] = {-0.6f, 0.0f, 0.0f, -0.2f, 0.35f, 0.0f,
                            0.2f,  -0.1f, 0.0f, 0.6f, 0.3f,  0.0f};
    clay_tube_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.point_type = CLAY_POINT_BSPLINE;
    p.radius_start = 0.14f;
    p.radius_mid = 0.09f;
    p.radius_end = 0.03f;
    p.tolerance = 0.01f;

    clay_item* round_tube = clay_tube_create(path, 4, &p, -1, nullptr, 0);
    REQUIRE(round_tube != nullptr);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, round_tube, nullptr) == CLAY_OK);
    clay_item_destroy(round_tube);
    float scale = 0.0f;
    REQUIRE(clay_safe_step_scale(doc, &scale) == CLAY_OK);
    CHECK(scale == doctest::Approx(1.0f));  // a swept sphere is exact
    clay_document_destroy(doc);

    SUBCASE("a profile makes it a swept item, and costs step scale") {
        const float box[2] = {0.09f, 0.05f};
        clay_item* shaped = clay_tube_create(path, 4, &p, CLAY_PROFILE_BOX, box, 2);
        REQUIRE(shaped != nullptr);
        clay_document* d2 = clay_document_create();
        clay_layer_id l2 = 0;
        REQUIRE(clay_add_sdf_layer(d2, "l", &l2) == CLAY_OK);
        REQUIRE(clay_layer_add_item(d2, l2, shaped, nullptr) == CLAY_OK);
        clay_item_destroy(shaped);
        float s2 = 0.0f;
        REQUIRE(clay_safe_step_scale(d2, &s2) == CLAY_OK);
        CHECK(s2 < 1.0f);
        clay_document_destroy(d2);
    }

    SUBCASE("and it refuses what is not a path") {
        CHECK(clay_tube_create(path, 1, &p, -1, nullptr, 0) == nullptr);
        clay_tube_params zero = p;
        zero.radius_start = zero.radius_mid = zero.radius_end = 0.0f;
        CHECK(clay_tube_create(path, 4, &zero, -1, nullptr, 0) == nullptr);
    }
}
