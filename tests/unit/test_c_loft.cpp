#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

#include "clay.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

// The C ABI loft surface (c-abi spec). A loft built through the boundary and
// one built on the engine types must give the same field.

using namespace clay;
using kernel::cf2;
using kernel::cf3;

namespace {

float eval_c(clay_document* doc, kernel::cfloat3 p) {
    float point[3] = {p.x, p.y, p.z};
    float out = 0.0f;
    REQUIRE(clay_eval_points(doc, "cpu", point, 1, &out, nullptr) == CLAY_OK);
    return out;
}

}  // namespace

TEST_CASE("c loft: a circle to a polygon matches the engine") {
    const float params[2] = {1.0f, 0.0f};  // half-depth, ease
    clay_item* item = clay_item_create(CLAY_PRIM_LOFT, params, 2);
    REQUIRE(item != nullptr);

    const float circle[1] = {0.9f};
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
            CLAY_OK);
    const float square[8] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_POLYGON, nullptr, 0, square, 4) ==
            CLAY_OK);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    scene::Document ref;
    scene::Layer& l = ref.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::loft(1.0f, 0);
    n.profiles = {scene::Profile::circle(0.9f), scene::Profile::polygon()};
    n.profile_polygons = {{}, {cf2(-0.5f, -0.5f), cf2(0.5f, -0.5f), cf2(0.5f, 0.5f),
                               cf2(-0.5f, 0.5f)}};
    l.sdf->insert(std::move(n));
    scene::Tape tape = scene::compile_document(ref);

    for (kernel::cfloat3 p : {cf3(0.85f, 0, -0.99f), cf3(0.45f, 0.45f, 0.99f),
                              cf3(0.7f, 0.7f, 0.99f), cf3(0, 0, 0), cf3(2, 2, 2)})
        CHECK(eval_c(doc, p) == doctest::Approx(tape.eval(p).d));

    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c loft: three profiles pinch in the middle") {
    const float params[2] = {1.0f, 0.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_LOFT, params, 2);
    REQUIRE(item != nullptr);
    const float wide[1] = {1.0f};
    const float narrow[1] = {0.2f};
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, wide, 1, nullptr, 0) == CLAY_OK);
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, narrow, 1, nullptr, 0) ==
            CLAY_OK);
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, wide, 1, nullptr, 0) == CLAY_OK);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    CHECK(eval_c(doc, cf3(0.9f, 0, -0.99f)) < 0.0f);
    CHECK(eval_c(doc, cf3(0.5f, 0, 0.0f)) > 0.0f);   // the waist
    CHECK(eval_c(doc, cf3(0.15f, 0, 0.0f)) < 0.0f);

    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c loft: invalid uses are refused") {
    const float params[2] = {1.0f, 0.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_LOFT, params, 2);
    REQUIRE(item != nullptr);
    const float circle[1] = {0.5f};

    CHECK(clay_item_add_loft_profile(nullptr, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_add_loft_profile(item, 99, circle, 1, nullptr, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // A polygon profile needs vertices, not parameters.
    CHECK(clay_item_add_loft_profile(item, CLAY_PROFILE_POLYGON, nullptr, 0, nullptr, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    const float two[4] = {0, 0, 1, 1};
    CHECK(clay_item_add_loft_profile(item, CLAY_PROFILE_POLYGON, nullptr, 0, two, 2) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A loft with fewer than two profiles is refused when it is PLACED: the
    // tape would otherwise read a record that was never written.
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
            CLAY_OK);
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
            CLAY_OK);
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    // Loft profiles belong to a loft, and a lift's single profile does not.
    const float depth[1] = {0.5f};
    clay_item* extrude = clay_item_create(CLAY_PRIM_EXTRUDE, depth, 1);
    REQUIRE(extrude != nullptr);
    CHECK(clay_item_add_loft_profile(extrude, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_profile(item, CLAY_PROFILE_CIRCLE, circle, 1) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // The flat descriptor cannot express a loft: its profiles are out of line.
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_LOFT;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.0f;
    CHECK(clay_add_item(doc, layer, &desc, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_item_destroy(extrude);
    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c loft: set_prim cannot turn a node into a loft") {
    // clay_layer_set_prim replaces only Node::prim, so it has no way to supply
    // profiles. It used to accept CLAY_PRIM_LOFT anyway, leaving a loft with
    // zero profiles that the tape read as a record never written — evaluating
    // the document segfaulted.
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);

    const float radius[1] = {1.0f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(sphere != nullptr);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, sphere, &node) == CLAY_OK);

    const float loft[2] = {1.0f, 0.0f};
    CHECK(clay_layer_set_prim(doc, layer, node, CLAY_PRIM_LOFT, loft, 2) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // The other primitives that carry out-of-line data are refused too.
    const float one[1] = {0.5f};
    CHECK(clay_layer_set_prim(doc, layer, node, CLAY_PRIM_STROKE, one, 1) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_prim(doc, layer, node, CLAY_PRIM_EXTRUDE, one, 1) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_prim(doc, layer, node, CLAY_PRIM_SWEPT, loft, 2) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // The node still evaluates, and an ordinary replacement still works.
    float d = 0.0f;
    const float origin[3] = {0, 0, 0};
    CHECK(clay_eval_points(doc, nullptr, origin, 1, &d, nullptr) == CLAY_OK);
    CHECK(d == doctest::Approx(-1.0f));
    const float box[3] = {0.5f, 0.5f, 0.5f};
    CHECK(clay_layer_set_prim(doc, layer, node, CLAY_PRIM_BOX, box, 3) == CLAY_OK);

    clay_item_destroy(sphere);
    clay_document_destroy(doc);
}

TEST_CASE("loft tape: fewer than two profiles evaluates far, not out of bounds") {
    // A document off disk can carry any profile count; the kernel used to read
    // two records unconditionally, dereferencing a null blob at count 0.
    for (int profiles = 0; profiles < 2; ++profiles) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n;
        n.prim = scene::Prim::loft(0.7f, 0);
        for (int i = 0; i < profiles; ++i) n.profiles.push_back(scene::Profile::circle(0.75f));
        l.sdf->insert(n);

        std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
        std::optional<scene::Document> back =
            scene::deserialize_document(bytes.data(), bytes.size(), scene::kSceneMinor);
        REQUIRE(back.has_value());

        scene::Tape tape = scene::compile_document(*back);
        const float d = tape.eval(cf3(0.2f, 0.0f, 0.0f)).d;
        CHECK(std::isfinite(d));
        CHECK(d > 0.0f);  // a loft that cannot be interpolated contributes nothing
    }
}
