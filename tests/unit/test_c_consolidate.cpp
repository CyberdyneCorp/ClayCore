#include <doctest/doctest.h>

#include <cstddef>

#include <cstring>

#include "clay.h"

// Consolidating a degraded chain across the C ABI (c-abi spec,
// add-consolidation-policy). The addition is purely additive, so what this
// checks is that the four new entry points say the same things the engine
// says, that the cost is knowable before it is paid, and that a protected
// layer is refused without being resampled first.

namespace {

clay_document* fresh_document(clay_layer_id* out_layer) {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    REQUIRE(clay_add_sdf_layer(doc, "l", out_layer) == CLAY_OK);
    return doc;
}

clay_node_id add_sphere(clay_document* doc, clay_layer_id layer, float radius, float x) {
    clay_item_desc d{};
    d.struct_size = sizeof(d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = radius;
    d.position[0] = x;
    d.rotation[3] = 1.0f;
    d.scale = 1.0f;
    d.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &d, &node) == CLAY_OK);
    return node;
}

clay_consolidation_params params_at(float cell, float band) {
    clay_consolidation_params p{};
    p.struct_size = sizeof(p);
    p.cell_size = cell;
    p.band = band;
    return p;
}

}  // namespace

TEST_CASE("the C ABI reports a chain's degradation and what caused it") {
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    add_sphere(doc, layer, 1.0f, 0.0f);

    clay_field_report clean{};
    clean.struct_size = sizeof(clean);
    REQUIRE(clay_layer_field_report(doc, layer, 0.25f, &clean) == CLAY_OK);
    CHECK(clean.safe_step_scale == doctest::Approx(1.0f));
    CHECK(clean.item_count == 1);
    CHECK(clean.advises_consolidation == 0);

    // Nine drags, the Move stroke's failure mode: each one is another grab on
    // the chain, and those multiply, so the decay is geometric.
    clay_move_params move{};
    move.struct_size = sizeof(move);
    move.radius = 0.5f;
    for (int i = 0; i < 9; ++i) {
        const float centre[3] = {1.0f + 0.25f * static_cast<float>(i), 0.0f, 0.0f};
        const float displacement[3] = {0.25f, 0.0f, 0.0f};
        size_t applied = 0;
        REQUIRE(clay_layer_move_surface(doc, layer, centre, displacement, &move, &applied) ==
                CLAY_OK);
        REQUIRE(applied == 1);
    }

    clay_field_report degraded{};
    degraded.struct_size = sizeof(degraded);
    REQUIRE(clay_layer_field_report(doc, layer, 0.25f, &degraded) == CLAY_OK);
    CHECK(degraded.longest_deformer_chain == 9);
    CHECK(degraded.steepest_volume == doctest::Approx(1.0f));  // no volume involved
    CHECK(degraded.safe_step_scale < 0.05f);
    // NOT advised, and issue #387 is why. The bound really is this bad, but
    // the layer is ONE analytic item: the bake wins back no edit list and no
    // stacked volume, and swaps a cheap primitive for a dense one. Measured on
    // a real gesture, a 29x better step scale and a 6x SLOWER gesture. The
    // advisory names the cure that applies rather than the symptom.
    CHECK(degraded.advises_consolidation == 0);
    CHECK(degraded.degradation == CLAY_DEGRADATION_DEFORMERS);
    CHECK(degraded.steepest_deformer_chain > 1.0f);
    CHECK(degraded.drawable_count == 1);

    // The advice is the CALLER's threshold, not the engine's opinion.
    clay_field_report unjudged{};
    unjudged.struct_size = sizeof(unjudged);
    REQUIRE(clay_layer_field_report(doc, layer, 0.0f, &unjudged) == CLAY_OK);
    CHECK(unjudged.advises_consolidation == 0);

    CHECK(unjudged.degradation == CLAY_DEGRADATION_NONE);

    // A caller built against the ORIGINAL struct — before the three fields
    // 0.70.0 appended — still works, and nothing is written past the end of
    // the struct it actually owns.
    struct original_report {
        uint32_t struct_size;
        float lipschitz;
        float safe_step_scale;
        float steepest_volume;
        int32_t longest_deformer_chain;
        int32_t item_count;
        int32_t advises_consolidation;
        uint32_t canary;
    };
    original_report old_shape{};
    old_shape.struct_size = static_cast<uint32_t>(offsetof(original_report, canary));
    old_shape.canary = 0xC0FFEEu;
    REQUIRE(clay_layer_field_report(doc, layer, 0.25f, reinterpret_cast<clay_field_report*>(
                                                          &old_shape)) == CLAY_OK);
    CHECK(old_shape.longest_deformer_chain == 9);
    CHECK(old_shape.canary == 0xC0FFEEu);  // untouched

    CHECK(clay_layer_field_report(doc, 999, 0.25f, &unjudged) == CLAY_ERROR_NOT_FOUND);
    clay_document_destroy(doc);
}

TEST_CASE("the C ABI advises consolidation when there IS an edit list to absorb") {
    // The other side of #387: the same deep chain over twenty items is worth
    // baking, because the bake wins back the cost of walking them.
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    for (int i = 0; i < 20; ++i)
        add_sphere(doc, layer, 0.4f, 0.25f * static_cast<float>(i) - 2.5f);

    clay_move_params move{};
    move.struct_size = sizeof(move);
    move.radius = 0.5f;
    const float centre[3] = {0.0f, 0.4f, 0.0f};
    const float displacement[3] = {0.0f, 0.9f, 0.0f};
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(doc, layer, centre, displacement, &move, &applied) ==
            CLAY_OK);

    clay_field_report r{};
    r.struct_size = sizeof(r);
    REQUIRE(clay_layer_field_report(doc, layer, 0.5f, &r) == CLAY_OK);
    REQUIRE(r.safe_step_scale < 0.5f);
    CHECK(r.drawable_count == 20);
    CHECK(r.degradation == CLAY_DEGRADATION_BOTH);
    CHECK(r.advises_consolidation == 1);
    clay_document_destroy(doc);
}

TEST_CASE("the C ABI quotes the cost before the document changes, and it is the bill") {
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    add_sphere(doc, layer, 0.6f, 0.0f);
    const clay_consolidation_params p = params_at(0.04f, 0.16f);

    clay_consolidation_cost quoted{};
    quoted.struct_size = sizeof(quoted);
    REQUIRE(clay_layer_consolidation_cost(doc, layer, &p, nullptr, nullptr, &quoted) == CLAY_OK);
    CHECK(quoted.brick_count > 0);
    CHECK(quoted.sample_count > 0);
    CHECK(quoted.bytes > 0);
    CHECK(quoted.cell_size == doctest::Approx(0.04f));
    CHECK(quoted.band == doctest::Approx(0.16f));
    CHECK(quoted.sample_lipschitz <= 1.10f);
    CHECK(quoted.safe_step_scale > 0.5f);
    CHECK(quoted.bounds_max[0] > quoted.bounds_min[0]);

    // Quoting did not consolidate anything.
    int32_t baked = 1;
    REQUIRE(clay_layer_consolidation_state(doc, layer, &baked, nullptr) == CLAY_OK);
    CHECK(baked == 0);

    clay_consolidation_cost paid{};
    paid.struct_size = sizeof(paid);
    REQUIRE(clay_layer_consolidate(doc, layer, &p, nullptr, nullptr, &paid) == CLAY_OK);
    CHECK(paid.brick_count == quoted.brick_count);
    CHECK(paid.bytes == quoted.bytes);

    clay_consolidation_cost state{};
    state.struct_size = sizeof(state);
    REQUIRE(clay_layer_consolidation_state(doc, layer, &baked, &state) == CLAY_OK);
    CHECK(baked == 1);
    CHECK(state.cell_size == doctest::Approx(0.04f));
    CHECK(state.brick_count == paid.brick_count);

    // A cell size is required: a layer has no intrinsic scale to derive one.
    clay_consolidation_params nocell = params_at(0.0f, 0.16f);
    CHECK(clay_layer_consolidation_cost(doc, layer, &nocell, nullptr, nullptr, &quoted) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_document_destroy(doc);
}

TEST_CASE("consolidating across the C ABI is one undo step that restores the items") {
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
    add_sphere(doc, layer, 0.6f, 0.0f);
    add_sphere(doc, layer, 0.3f, 0.55f);

    size_t before_depth = 0;
    REQUIRE(clay_document_undo_state(doc, nullptr, &before_depth, nullptr) == CLAY_OK);

    const clay_consolidation_params p = params_at(0.04f, 0.16f);
    REQUIRE(clay_layer_consolidate(doc, layer, &p, nullptr, nullptr, nullptr) == CLAY_OK);

    size_t after_depth = 0;
    REQUIRE(clay_document_undo_state(doc, nullptr, &after_depth, nullptr) == CLAY_OK);
    CHECK(after_depth == before_depth + 1);  // ONE step, two items absorbed

    int32_t undone = 0;
    REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    int32_t baked = 1;
    REQUIRE(clay_layer_consolidation_state(doc, layer, &baked, nullptr) == CLAY_OK);
    CHECK(baked == 0);

    // The parametric form is back: the second sphere's surface is where it was.
    const float point[3] = {0.84f, 0.0f, 0.0f};
    float d = 1.0f;
    REQUIRE(clay_eval_points(doc, "cpu", point, 1, &d, nullptr) == CLAY_OK);
    CHECK(d == doctest::Approx(0.0f).epsilon(0.0).scale(1.0f).epsilon(0.03));
    clay_document_destroy(doc);
}

TEST_CASE("a protected layer refuses to consolidate") {
    for (int locked = 0; locked < 2; ++locked) {
        clay_layer_id layer = 0;
        clay_document* doc = fresh_document(&layer);
        add_sphere(doc, layer, 0.6f, 0.0f);
        REQUIRE(clay_document_set_layer_protection(doc, layer, locked ? 0 : 1, locked) ==
                CLAY_OK);

        const clay_consolidation_params p = params_at(0.05f, 0.18f);
        CHECK(clay_layer_consolidate(doc, layer, &p, nullptr, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        int32_t baked = 1;
        REQUIRE(clay_layer_consolidation_state(doc, layer, &baked, nullptr) == CLAY_OK);
        CHECK(baked == 0);
        clay_document_destroy(doc);
    }
}

TEST_CASE("an explicit region pins where a repeated consolidation samples") {
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    add_sphere(doc, layer, 0.6f, 0.0f);
    const clay_consolidation_params p = params_at(0.04f, 0.16f);
    const float lo[3] = {-1.0f, -1.0f, -1.0f};
    const float hi[3] = {1.0f, 1.0f, 1.0f};

    clay_consolidation_cost first{};
    first.struct_size = sizeof(first);
    REQUIRE(clay_layer_consolidate(doc, layer, &p, lo, hi, &first) == CLAY_OK);
    clay_consolidation_cost second{};
    second.struct_size = sizeof(second);
    REQUIRE(clay_layer_consolidate(doc, layer, &p, lo, hi, &second) == CLAY_OK);

    // Pinned, the box does not creep outwards by two paddings a bake.
    for (int a = 0; a < 3; ++a) {
        CHECK(second.bounds_min[a] == doctest::Approx(first.bounds_min[a]));
        CHECK(second.bounds_max[a] == doctest::Approx(first.bounds_max[a]));
    }
    // And the bound holds across the repeat, which is the whole claim.
    CHECK(second.sample_lipschitz <= 1.10f);
    CHECK(second.bytes <= first.bytes * 2);
    clay_document_destroy(doc);
}
