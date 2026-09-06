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

// -- merging a bake into a REGION of a layer (issue #390) --------------------

TEST_CASE("the C ABI merges a region and leaves the rest parametric") {
    // A row of balls far enough apart that each one's influence is its own.
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    for (int i = 0; i < 4; ++i) add_sphere(doc, layer, 0.5f, static_cast<float>(i) * 3.0f);

    const float lo[3] = {-0.6f, -0.6f, -0.6f};
    const float hi[3] = {0.6f, 0.6f, 0.6f};

    // What it WOULD take, before anything is baked.
    clay_region_merge plan{};
    plan.struct_size = sizeof(plan);
    REQUIRE(clay_layer_plan_region_merge(doc, layer, lo, hi, &plan) == CLAY_OK);
    CHECK(plan.absorbed == 1);
    CHECK(plan.whole_layer == 0);
    CHECK(plan.box_max[0] < 2.0f);  // it did not reach the ball at x = 3

    const clay_consolidation_params p = params_at(0.02f, 0.08f);
    clay_consolidation_cost cost{};
    cost.struct_size = sizeof(cost);
    clay_region_merge done{};
    done.struct_size = sizeof(done);
    REQUIRE(clay_layer_consolidate_region(doc, layer, lo, hi, &p, &cost, &done) == CLAY_OK);
    CHECK(done.absorbed == 1);
    CHECK(done.whole_layer == 0);
    CHECK(cost.brick_count > 0);

    // One absorbed, one volume back in its place, three still parametric.
    size_t nodes = 0;
    REQUIRE(clay_layer_node_count(doc, layer, &nodes) == CLAY_OK);
    CHECK(nodes == 4);
    // ...and the layer is NOT reported consolidated, because it is not: the
    // three items outside the patch still carry their parameters.
    int32_t consolidated = 1;
    REQUIRE(clay_layer_consolidation_state(doc, layer, &consolidated, nullptr) == CLAY_OK);
    CHECK(consolidated == 0);
    clay_document_destroy(doc);
}

TEST_CASE("the C ABI keeps one baked item however many gestures work the patch") {
    // The measurement issue #390 filed: appending a volume per gesture is O(n)
    // in gestures, because every later bake samples all the earlier ones.
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    for (int i = 0; i < 4; ++i) add_sphere(doc, layer, 0.5f, static_cast<float>(i) * 3.0f);

    const float lo[3] = {-0.6f, -0.6f, -0.6f};
    const float hi[3] = {0.6f, 0.6f, 0.6f};
    const clay_consolidation_params p = params_at(0.02f, 0.08f);

    for (int gesture = 1; gesture <= 6; ++gesture) {
        CAPTURE(gesture);
        REQUIRE(clay_layer_consolidate_region(doc, layer, lo, hi, &p, nullptr, nullptr) ==
                CLAY_OK);
        size_t nodes = 0;
        REQUIRE(clay_layer_node_count(doc, layer, &nodes) == CLAY_OK);
        CHECK(nodes == 4);  // not 4 + gesture
    }
    clay_document_destroy(doc);
}

TEST_CASE("the C ABI refuses a region merge it cannot make sense of") {
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    add_sphere(doc, layer, 0.5f, 0.0f);
    const float lo[3] = {-0.6f, -0.6f, -0.6f};
    const float hi[3] = {0.6f, 0.6f, 0.6f};
    const clay_consolidation_params p = params_at(0.02f, 0.08f);

    // No region: a region merge without one is a whole-layer consolidate, and a
    // host should ask for that by name rather than get it by omission.
    CHECK(clay_layer_consolidate_region(doc, layer, nullptr, hi, &p, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_consolidate_region(doc, layer, lo, nullptr, &p, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // An inverted box is empty, not a region.
    CHECK(clay_layer_consolidate_region(doc, layer, hi, lo, &p, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_consolidate_region(doc, 999, lo, hi, &p, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_consolidate_region(doc, layer, lo, hi, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A region over empty space reaches no item.
    const float far_lo[3] = {40.0f, 40.0f, 40.0f};
    const float far_hi[3] = {41.0f, 41.0f, 41.0f};
    CHECK(clay_layer_consolidate_region(doc, layer, far_lo, far_hi, &p, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A stale struct_size on the report is refused rather than half-filled.
    clay_region_merge stale{};
    stale.struct_size = 0;
    CHECK(clay_layer_plan_region_merge(doc, layer, lo, hi, &stale) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_document_destroy(doc);
}

// -- the advice a host can act on (advise-a-consolidation) -------------------

namespace {

// A layer degraded by BOTH mechanisms, so the bake really is the cure: twenty
// spheres plus one deep grab, the #387 fixture above.
clay_document* absorbable_chain(clay_layer_id* out_layer) {
    clay_document* doc = fresh_document(out_layer);
    for (int i = 0; i < 20; ++i)
        add_sphere(doc, *out_layer, 0.4f, 0.25f * static_cast<float>(i) - 2.5f);
    clay_move_params move{};
    move.struct_size = sizeof(move);
    move.radius = 0.5f;
    const float centre[3] = {0.0f, 0.4f, 0.0f};
    const float displacement[3] = {0.0f, 0.9f, 0.0f};
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(doc, *out_layer, centre, displacement, &move, &applied) ==
            CLAY_OK);
    return doc;
}

}  // namespace

TEST_CASE("the C ABI advises a resolution, and baking at it cures what was reported") {
    // THE PROPERTY. The flag said "bake"; this says at what, and the bake at
    // that number really does lift the layer out of the state the flag named.
    clay_layer_id layer = 0;
    clay_document* doc = absorbable_chain(&layer);
    const float threshold = 0.5f;

    clay_field_report before{};
    before.struct_size = sizeof(before);
    REQUIRE(clay_layer_field_report(doc, layer, threshold, &before) == CLAY_OK);
    REQUIRE(before.advises_consolidation == 1);

    clay_consolidation_params advised{};
    advised.struct_size = sizeof(advised);
    clay_consolidation_cost projected{};
    projected.struct_size = sizeof(projected);
    int32_t advises = -1;
    REQUIRE(clay_layer_consolidation_advice(doc, layer, threshold, &advised, &projected,
                                            &advises) == CLAY_OK);
    CHECK(advises == 1);
    CHECK(advised.cell_size > 0.0f);
    CHECK(advised.band == doctest::Approx(3.0f * advised.cell_size));
    CHECK(advised.padding == doctest::Approx(advised.band));
    CHECK(advised.skip_redistance == 0);  // redistancing IS the cure
    CHECK(projected.brick_count > 0);
    CHECK(projected.bytes > 0);
    CHECK(projected.safe_step_scale >= threshold);

    // The bill quoted at the advised number is the bill the cost query gives
    // for the same params — the advice is not a second, looser estimate.
    clay_consolidation_cost requoted{};
    requoted.struct_size = sizeof(requoted);
    REQUIRE(clay_layer_consolidation_cost(doc, layer, &advised, nullptr, nullptr, &requoted) ==
            CLAY_OK);
    CHECK(requoted.brick_count == projected.brick_count);
    CHECK(requoted.bytes == projected.bytes);
    CHECK(requoted.safe_step_scale == doctest::Approx(projected.safe_step_scale));

    REQUIRE(clay_layer_consolidate(doc, layer, &advised, nullptr, nullptr, nullptr) == CLAY_OK);
    clay_field_report after{};
    after.struct_size = sizeof(after);
    REQUIRE(clay_layer_field_report(doc, layer, threshold, &after) == CLAY_OK);
    CHECK(after.advises_consolidation == 0);
    CHECK(after.degradation == CLAY_DEGRADATION_NONE);
    CHECK(after.safe_step_scale >= threshold);
    CHECK(after.safe_step_scale == doctest::Approx(projected.safe_step_scale));
    clay_document_destroy(doc);
}

TEST_CASE("not advised zeroes the params, so ignoring the verdict fails loudly") {
    clay_layer_id layer = 0;
    clay_document* doc = absorbable_chain(&layer);

    // 0.8 is above 1/sqrt(3) = 0.577, which is the best a redistanced volume
    // can declare. No bake reaches it, so handing over params would trade a
    // parametric layer for a dense one and still miss the budget.
    clay_consolidation_params advised{};
    advised.struct_size = sizeof(advised);
    advised.cell_size = 12345.0f;  // whatever the caller's buffer held
    clay_consolidation_cost projected{};
    projected.struct_size = sizeof(projected);
    projected.brick_count = 99;
    int32_t advises = -1;
    REQUIRE(clay_layer_consolidation_advice(doc, layer, 0.8f, &advised, &projected, &advises) ==
            CLAY_OK);
    CHECK(advises == 0);
    CHECK(advised.cell_size == 0.0f);
    CHECK(advised.struct_size == sizeof(advised));  // theirs, preserved
    CHECK(projected.brick_count == 0);

    // A host that never read the verdict is refused by the NEXT call rather
    // than baking at a resolution nobody chose, and the document is unchanged.
    clay_blob* was = nullptr;
    REQUIRE(clay_document_save_memory(doc, &was) == CLAY_OK);
    CHECK(clay_layer_consolidate(doc, layer, &advised, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_blob* now = nullptr;
    REQUIRE(clay_document_save_memory(doc, &now) == CLAY_OK);
    REQUIRE(clay_blob_size(was) == clay_blob_size(now));
    CHECK(std::memcmp(clay_blob_data(was), clay_blob_data(now), clay_blob_size(was)) == 0);
    clay_blob_destroy(was);
    clay_blob_destroy(now);
    clay_document_destroy(doc);
}

TEST_CASE("a deformer-degraded layer is advised nothing across the C ABI") {
    // #387's case, one step further along: the report withholds the flag and
    // the advice withholds the params, so there is nothing to pass on.
    clay_layer_id layer = 0;
    clay_document* doc = fresh_document(&layer);
    add_sphere(doc, layer, 1.0f, 0.0f);
    clay_move_params move{};
    move.struct_size = sizeof(move);
    move.radius = 0.5f;
    const float centre[3] = {1.0f, 0.0f, 0.0f};
    const float displacement[3] = {0.9f, 0.0f, 0.0f};
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(doc, layer, centre, displacement, &move, &applied) == CLAY_OK);

    clay_field_report r{};
    r.struct_size = sizeof(r);
    REQUIRE(clay_layer_field_report(doc, layer, 0.5f, &r) == CLAY_OK);
    REQUIRE(r.degradation == CLAY_DEGRADATION_DEFORMERS);

    clay_consolidation_params advised{};
    advised.struct_size = sizeof(advised);
    int32_t advises = -1;
    REQUIRE(clay_layer_consolidation_advice(doc, layer, 0.5f, &advised, nullptr, &advises) ==
            CLAY_OK);
    CHECK(advises == 0);
    CHECK(advised.cell_size == 0.0f);
    clay_document_destroy(doc);
}

TEST_CASE("asking for the advice bakes nothing and does not unlink a subtool") {
    clay_layer_id source = 0;
    clay_document* doc = absorbable_chain(&source);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);

    clay_blob* was = nullptr;
    REQUIRE(clay_document_save_memory(doc, &was) == CLAY_OK);

    clay_consolidation_params advised{};
    advised.struct_size = sizeof(advised);
    int32_t advises = -1;
    REQUIRE(clay_layer_consolidation_advice(doc, source, 0.5f, &advised, nullptr, &advises) ==
            CLAY_OK);
    REQUIRE(advises == 1);

    // Byte-identical: asking whether a bake is ADVISABLE must no more change a
    // document than asking what one costs does.
    clay_blob* now = nullptr;
    REQUIRE(clay_document_save_memory(doc, &now) == CLAY_OK);
    REQUIRE(clay_blob_size(was) == clay_blob_size(now));
    CHECK(std::memcmp(clay_blob_data(was), clay_blob_data(now), clay_blob_size(was)) == 0);
    clay_blob_destroy(was);
    clay_blob_destroy(now);

    // ...and the sharing is still reported from both ends.
    clay_layer_info info{};
    info.struct_size = sizeof(info);
    REQUIRE(clay_document_layer_info(doc, instance, &info) == CLAY_OK);
    CHECK(info.content_source == source);
    CHECK(info.share_count == 2u);
    REQUIRE(clay_document_layer_info(doc, source, &info) == CLAY_OK);
    CHECK(info.share_count == 2u);
    clay_document_destroy(doc);
}

TEST_CASE("the advice refuses before it samples, and a mixed stack is an answer") {
    clay_layer_id layer = 0;
    clay_document* doc = absorbable_chain(&layer);
    clay_consolidation_params advised{};
    advised.struct_size = sizeof(advised);
    int32_t advises = -1;

    CHECK(clay_layer_consolidation_advice(nullptr, layer, 0.5f, &advised, nullptr, &advises) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_consolidation_advice(doc, layer, 0.5f, nullptr, nullptr, &advises) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_consolidation_advice(doc, layer, 0.5f, &advised, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // A zero threshold is legitimate on clay_layer_field_report and refused
    // here: every output of this call is defined against a threshold, so a
    // zero would buy a full sampling pass to be told nothing.
    CHECK(clay_layer_consolidation_advice(doc, layer, 0.0f, &advised, nullptr, &advises) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_consolidation_advice(doc, layer, -1.0f, &advised, nullptr, &advises) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // "No such layer" and "not advised" are different answers.
    CHECK(clay_layer_consolidation_advice(doc, 999, 0.5f, &advised, nullptr, &advises) ==
          CLAY_ERROR_NOT_FOUND);
    // A struct_size below the original layout is malformed, not retryable.
    clay_consolidation_params stunted{};
    stunted.struct_size = 4;
    CHECK(clay_layer_consolidation_advice(doc, layer, 0.5f, &stunted, nullptr, &advises) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A protected layer, and a layer consolidation does not apply to: success,
    // advises nothing, zeroed. A host walking a stack of mixed kinds should not
    // have to special-case them.
    REQUIRE(clay_document_set_layer_protection(doc, layer, 0, 1) == CLAY_OK);
    REQUIRE(clay_layer_consolidation_advice(doc, layer, 0.5f, &advised, nullptr, &advises) ==
            CLAY_OK);
    CHECK(advises == 0);
    CHECK(advised.cell_size == 0.0f);
    REQUIRE(clay_document_set_layer_protection(doc, layer, 0, 0) == CLAY_OK);

    clay_layer_id empty = 0;
    REQUIRE(clay_add_sdf_layer(doc, "blank", &empty) == CLAY_OK);
    REQUIRE(clay_layer_consolidation_advice(doc, empty, 0.5f, &advised, nullptr, &advises) ==
            CLAY_OK);
    CHECK(advises == 0);
    clay_document_destroy(doc);
}

TEST_CASE("an older caller gets its own fields of the advice and nothing past them") {
    clay_layer_id layer = 0;
    clay_document* doc = absorbable_chain(&layer);

    // Both descriptors are at their ORIGINAL layout today, so "older" is that
    // layout with a canary behind it. Appending a field later must not start
    // writing into it.
    struct original_params {
        uint32_t struct_size;
        float cell_size;
        float band;
        float padding;
        int32_t skip_redistance;
        uint32_t canary;
    };
    struct original_cost {
        uint32_t struct_size;
        float cell_size;
        float band;
        uint64_t brick_count;
        uint64_t sample_count;
        uint64_t bytes;
        float sample_lipschitz;
        float lipschitz;
        float safe_step_scale;
        float bounds_min[3];
        float bounds_max[3];
        uint32_t canary;
    };
    original_params p{};
    p.struct_size = static_cast<uint32_t>(offsetof(original_params, canary));
    p.canary = 0xC0FFEEu;
    original_cost c{};
    c.struct_size = static_cast<uint32_t>(offsetof(original_cost, canary));
    c.canary = 0xBADF00Du;
    int32_t advises = -1;
    REQUIRE(clay_layer_consolidation_advice(doc, layer, 0.5f,
                                            reinterpret_cast<clay_consolidation_params*>(&p),
                                            reinterpret_cast<clay_consolidation_cost*>(&c),
                                            &advises) == CLAY_OK);
    CHECK(advises == 1);
    CHECK(p.cell_size > 0.0f);
    CHECK(c.brick_count > 0);
    CHECK(p.canary == 0xC0FFEEu);
    CHECK(c.canary == 0xBADF00Du);
    CHECK(p.struct_size == static_cast<uint32_t>(offsetof(original_params, canary)));

    // ...and on the not-advised path, where the zeroing happens rather than a
    // fill: the zeroing is bounded by the caller's size too.
    REQUIRE(clay_layer_consolidation_advice(doc, layer, 0.8f,
                                            reinterpret_cast<clay_consolidation_params*>(&p),
                                            reinterpret_cast<clay_consolidation_cost*>(&c),
                                            &advises) == CLAY_OK);
    CHECK(advises == 0);
    CHECK(p.cell_size == 0.0f);
    CHECK(c.brick_count == 0);
    CHECK(p.canary == 0xC0FFEEu);
    CHECK(c.canary == 0xBADF00Du);
    clay_document_destroy(doc);
}

TEST_CASE("a NULL cost is not a fast path — the verdict is the projection") {
    // Stated in the header so nobody reads NULL as a cheap arm. The check here
    // is that the ANSWER is the same, which is the part a host can rely on.
    clay_layer_id layer = 0;
    clay_document* doc = absorbable_chain(&layer);

    clay_consolidation_params with{}, without{};
    with.struct_size = sizeof(with);
    without.struct_size = sizeof(without);
    clay_consolidation_cost cost{};
    cost.struct_size = sizeof(cost);
    int32_t a = -1, b = -1;
    REQUIRE(clay_layer_consolidation_advice(doc, layer, 0.5f, &with, &cost, &a) == CLAY_OK);
    REQUIRE(clay_layer_consolidation_advice(doc, layer, 0.5f, &without, nullptr, &b) == CLAY_OK);
    CHECK(a == b);
    CHECK(with.cell_size == doctest::Approx(without.cell_size));
    clay_document_destroy(doc);
}
