#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/scene/tape.h"

// The C ABI swept surface (c-abi spec). A sweep built through the boundary and
// one built on the engine types must give the same field — the guide's frames
// are transported at compile time, so any disagreement would mean the two
// paths built different guides.

using namespace clay;
using kernel::cf3;

namespace {

float eval_c(clay_document* doc, kernel::cfloat3 p) {
    float point[3] = {p.x, p.y, p.z};
    float out = 0.0f;
    REQUIRE(clay_eval_points(doc, "cpu", point, 1, &out, nullptr) == CLAY_OK);
    return out;
}

// x, y, z, radius per point — the radius is unused by a guide, which carries
// shape in its profiles rather than per point.
const float kBentGuide[12] = {-1, 0, 0, 0, 0, 0.7f, 0, 0, 1, 0, 0, 0};

}  // namespace

TEST_CASE("c swept: a sweep matches the engine") {
    const float ease[1] = {0.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SWEPT, ease, 1);
    REQUIRE(item != nullptr);

    const std::int32_t types[3] = {CLAY_POINT_SPLINE, CLAY_POINT_SPLINE, CLAY_POINT_SPLINE};
    REQUIRE(clay_item_set_curve_points(item, kBentGuide, 3, types, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_item_set_curve(item, 0, 0.02f) == CLAY_OK);
    const float wide[1] = {0.3f};
    const float narrow[1] = {0.12f};
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, wide, 1, nullptr, 0) == CLAY_OK);
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, narrow, 1, nullptr, 0) ==
            CLAY_OK);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    scene::Document ref;
    scene::Layer& l = ref.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::swept(0);
    for (int i = 0; i < 3; ++i) {
        scene::StrokePoint sp;
        sp.pos = cf3(kBentGuide[i * 4], kBentGuide[i * 4 + 1], kBentGuide[i * 4 + 2]);
        sp.type = scene::StrokePointType::Spline;
        n.stroke.push_back(sp);
    }
    n.curve_tolerance = 0.02f;
    n.profiles = {scene::Profile::circle(0.3f), scene::Profile::circle(0.12f)};
    n.profile_polygons = {{}, {}};
    l.sdf->insert(std::move(n));
    scene::Tape tape = scene::compile_document(ref);

    for (kernel::cfloat3 p : {cf3(-0.9f, 0.1f, 0), cf3(0, 0.8f, 0), cf3(0.9f, 0.05f, 0),
                              cf3(0, 0, 0), cf3(2, 2, 2)})
        CHECK(eval_c(doc, p) == doctest::Approx(tape.eval(p).d));

    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c swept: degenerate sweeps are refused where the item is placed") {
    const float ease[1] = {0.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SWEPT, ease, 1);
    REQUIRE(item != nullptr);
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);

    // Neither guide nor profiles.
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    const float circle[1] = {0.2f};
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
            CLAY_OK);
    REQUIRE(clay_item_add_loft_profile(item, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
            CLAY_OK);
    // Profiles but no guide.
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    const float one_point[4] = {0, 0, 0, 0};
    REQUIRE(clay_item_set_curve_points(item, one_point, 1, nullptr, nullptr, nullptr) == CLAY_OK);
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    REQUIRE(clay_item_set_curve_points(item, kBentGuide, 3, nullptr, nullptr, nullptr) ==
            CLAY_OK);
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    // A guide belongs to a stroke or a sweep, and nothing else.
    const float radius[1] = {1.0f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(sphere != nullptr);
    CHECK(clay_item_set_curve_points(sphere, kBentGuide, 3, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_add_loft_profile(sphere, CLAY_PROFILE_CIRCLE, circle, 1, nullptr, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A closed guide is refused rather than ignored: transport around a loop
    // does not close the seam, and silently dropping the flag would tell the
    // caller it had a closed sweep when it did not.
    CHECK(clay_item_set_curve(item, 1, 0.02f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_curve(item, 0, 0.02f) == CLAY_OK);

    clay_item_destroy(sphere);
    clay_item_destroy(item);
    clay_document_destroy(doc);
}

TEST_CASE("c swept: the flat descriptor cannot express one") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);

    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_SWEPT;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.0f;
    CHECK(clay_add_item(doc, layer, &desc, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_document_destroy(doc);
}
