#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// The C ABI document-grab surface (c-abi spec: add-document-grab). Everything
// here goes through the boundary — the document is never reached into — so this
// tests what a host actually gets.

namespace {

clay_grab_params drag(float cx, float cy, float cz, float radius, float dx, float dy, float dz) {
    clay_grab_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.centre[0] = cx;
    p.centre[1] = cy;
    p.centre[2] = cz;
    p.radius = radius;
    p.displacement[0] = dx;
    p.displacement[1] = dy;
    p.displacement[2] = dz;
    return p;
}

// Two overlapping balls blended into one form, built through the C ABI.
clay_document* two_balls() {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    for (float x : {-0.35f, 0.35f}) {
        float radius[1] = {0.5f};
        clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
        REQUIRE(item != nullptr);
        float where[3] = {x, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(item, where) == CLAY_OK);
        REQUIRE(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, 0.25f) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);
        clay_item_destroy(item);
    }
    return doc;
}

std::vector<float> eval_at(const clay_document* doc, const std::vector<float>& xyz) {
    std::vector<float> out(xyz.size() / 3);
    REQUIRE(clay_eval_points(doc, nullptr, xyz.data(), out.size(), out.data(), nullptr) ==
            CLAY_OK);
    return out;
}

// A column of probes down +Y at x, and where the surface crosses.
float top_at(const clay_document* doc, float x) {
    std::vector<float> pts;
    for (float y = 2.0f; y > -1.6f; y -= 0.004f) {
        pts.push_back(x);
        pts.push_back(y);
        pts.push_back(0.0f);
    }
    std::vector<float> d = eval_at(doc, pts);
    for (std::size_t i = 0; i < d.size(); ++i)
        if (d[i] <= 0.0f) return pts[i * 3 + 1];
    return -99.0f;
}

std::vector<float> probe_grid() {
    std::vector<float> pts;
    for (float x = -1.2f; x <= 1.2f; x += 0.061f)
        for (float y = -1.0f; y <= 1.2f; y += 0.071f) {
            pts.push_back(x);
            pts.push_back(y);
            pts.push_back(0.09f);
        }
    return pts;
}

}  // namespace

TEST_CASE("c abi: a drag moves the surface, not one item") {
    clay_document* plain = two_balls();
    clay_document* doc = two_balls();
    clay_grab_params p = drag(0, 0, 0, 1.2f, 0, 0.4f, 0);

    std::size_t reached = 0;
    REQUIRE(clay_document_grab(doc, &p, &reached) == CLAY_OK);
    CHECK(reached == 2);

    const float left = top_at(doc, -0.35f) - top_at(plain, -0.35f);
    const float right = top_at(doc, 0.35f) - top_at(plain, 0.35f);
    INFO("left rose " << left << ", right rose " << right);
    CHECK(left > 0.05f);
    CHECK(right > 0.05f);
    CHECK(left == doctest::Approx(right).epsilon(0.05));  // one surface, not two items

    clay_document_destroy(plain);
    clay_document_destroy(doc);
}

TEST_CASE("c abi: a drag coalesces rather than composing") {
    // The boundary-level statement of coalescing, and a better one than counting
    // deformers: three frames of one drag ending at 0.4 must give the same field
    // as a single drag of 0.4. Appending instead would compose three warps.
    clay_document* stepped = two_balls();
    for (float d : {0.1f, 0.25f, 0.4f}) {
        clay_grab_params p = drag(0, 0, 0, 1.2f, 0, d, 0);
        REQUIRE(clay_document_grab(stepped, &p, nullptr) == CLAY_OK);
    }
    clay_document* once = two_balls();
    {
        clay_grab_params p = drag(0, 0, 0, 1.2f, 0, 0.4f, 0);
        REQUIRE(clay_document_grab(once, &p, nullptr) == CLAY_OK);
    }

    std::vector<float> pts = probe_grid();
    std::vector<float> a = eval_at(stepped, pts), b = eval_at(once, pts);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(a[i] == doctest::Approx(b[i]));
    }

    SUBCASE("so the step scale does not decay with the frame count") {
        float stepped_scale = 0.0f, once_scale = 0.0f;
        REQUIRE(clay_safe_step_scale(stepped, &stepped_scale) == CLAY_OK);
        REQUIRE(clay_safe_step_scale(once, &once_scale) == CLAY_OK);
        CHECK(stepped_scale == doctest::Approx(once_scale));
    }

    clay_document_destroy(stepped);
    clay_document_destroy(once);
}

TEST_CASE("c abi: a different drag is a different deformer") {
    clay_document* doc = two_balls();
    clay_grab_params first = drag(0, 0, 0, 1.2f, 0, 0.3f, 0);
    REQUIRE(clay_document_grab(doc, &first, nullptr) == CLAY_OK);
    float after_one = 0.0f;
    REQUIRE(clay_safe_step_scale(doc, &after_one) == CLAY_OK);

    clay_grab_params second = drag(0.4f, 0.3f, 0, 0.8f, 0.2f, 0, 0);
    REQUIRE(clay_document_grab(doc, &second, nullptr) == CLAY_OK);
    float after_two = 0.0f;
    REQUIRE(clay_safe_step_scale(doc, &after_two) == CLAY_OK);

    // A second, unrelated drag is kept alongside the first, so the field gets
    // steeper rather than the first being silently replaced.
    INFO("step scale " << after_one << " -> " << after_two);
    CHECK(after_two < after_one);
    clay_document_destroy(doc);
}

TEST_CASE("c abi: previewing a drag touches nothing") {
    clay_document* doc = two_balls();
    clay_document* fresh = two_balls();
    clay_grab_params p = drag(0, 0, 0, 1.2f, 0, 0.4f, 0);

    std::size_t count = 0;
    REQUIRE(clay_document_grab_preview(doc, &p, nullptr, nullptr, 0, &count) == CLAY_OK);
    CHECK(count == 2);

    std::vector<std::uint32_t> layers(count), nodes(count);
    REQUIRE(clay_document_grab_preview(doc, &p, layers.data(), nodes.data(), count, &count) ==
            CLAY_OK);
    CHECK(layers[0] == layers[1]);
    CHECK(nodes[0] != nodes[1]);

    std::vector<float> pts = probe_grid();
    std::vector<float> a = eval_at(doc, pts), b = eval_at(fresh, pts);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(a[i] == doctest::Approx(b[i]));
    }

    clay_document_destroy(fresh);
    clay_document_destroy(doc);
}

TEST_CASE("c abi: a drag that reaches nothing is not an error") {
    clay_document* doc = two_balls();
    clay_grab_params p = drag(9, 9, 9, 0.5f, 0, 0.4f, 0);
    std::size_t reached = 1;
    CHECK(clay_document_grab(doc, &p, &reached) == CLAY_OK);
    CHECK(reached == 0);
    clay_document_destroy(doc);
}

TEST_CASE("c abi: a drag is one undo step") {
    clay_document* doc = two_balls();
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
    clay_grab_params p = drag(0, 0, 0, 1.2f, 0, 0.4f, 0);
    REQUIRE(clay_document_grab(doc, &p, nullptr) == CLAY_OK);

    std::int32_t enabled = 0;
    std::size_t undo_depth = 0, redo_depth = 0;
    REQUIRE(clay_document_undo_state(doc, &enabled, &undo_depth, &redo_depth) == CLAY_OK);
    CHECK(undo_depth == 1);  // two items reached, one gesture

    const float lifted = top_at(doc, 0.0f);
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
    CHECK(undone != 0);
    CHECK(top_at(doc, 0.0f) < lifted - 0.05f);  // the whole gesture came back off

    clay_document_destroy(doc);
}

TEST_CASE("c abi: the descriptor is versioned like every other") {
    clay_document* doc = two_balls();
    clay_grab_params p = drag(0, 0, 0, 1.0f, 0, 0.2f, 0);
    p.struct_size = 0;  // never a valid descriptor size
    CHECK(clay_document_grab(doc, &p, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_grab(doc, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_grab(nullptr, &p, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_document_destroy(doc);
}
