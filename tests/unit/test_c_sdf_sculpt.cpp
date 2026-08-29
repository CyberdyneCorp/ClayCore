#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "clay.h"

// Transient SDF sculpt transactions across the C ABI (c-abi spec,
// add-sdf-sculpt-transaction).
//
// The claim the ABI has to carry is the same one the C++ side makes: between
// begin and commit the DOCUMENT does not change. A save taken mid-gesture must
// be the save taken before it, byte for byte, and that is what these check
// first — a preview that looks right while quietly editing the document is the
// defect the whole feature replaces.

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

clay_sculpt_policy sculpt_policy(float cell) {
    clay_sculpt_policy p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.cell_size = cell;
    return p;
}

clay_move_params move_params(float radius) {
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.radius = radius;
    return p;
}

clay_relax_params relax_params(float x, float y, float z, float region) {
    clay_relax_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.strength = 0.8f;
    p.radius_cells = 1;
    p.iterations = 1;
    p.centre[0] = x;
    p.centre[1] = y;
    p.centre[2] = z;
    p.region_radius = region;
    return p;
}

clay_sculpt_dirty dirty_out() {
    clay_sculpt_dirty d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<uint32_t>(sizeof d);
    return d;
}

clay_sculpt_budget budget_out() {
    clay_sculpt_budget b;
    std::memset(&b, 0, sizeof b);
    b.struct_size = static_cast<uint32_t>(sizeof b);
    return b;
}

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

std::vector<uint8_t> saved(const clay_document* doc) {
    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc, &blob) == CLAY_OK);
    const uint8_t* data = clay_blob_data(blob);
    const size_t size = clay_blob_size(blob);
    std::vector<uint8_t> out(data, data + size);
    clay_blob_destroy(blob);
    return out;
}

size_t undo_depth(const clay_document* doc) {
    size_t depth = 0;
    REQUIRE(clay_document_undo_state(doc, nullptr, &depth, nullptr) == CLAY_OK);
    return depth;
}

}  // namespace

TEST_CASE("c abi: a live Smooth previews without touching the document") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const std::vector<uint8_t> before = saved(d.doc);

    const clay_sculpt_policy policy = sculpt_policy(0.05f);
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
    REQUIRE(tx != nullptr);
    CHECK(saved(d.doc) == before);

    clay_sculpt_dirty dirty = dirty_out();
    const clay_relax_params dab = relax_params(0.0f, 0.42f, 0.0f, 0.3f);
    REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, &dirty) == CLAY_OK);
    CHECK(dirty.changed == 1);
    CHECK(dirty.has_bounds == 1);
    CHECK(dirty.touched_bricks > 0);
    CHECK(dirty.bounds_min[0] < dirty.bounds_max[0]);

    for (int i = 0; i < 20; ++i)
        REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);
    CHECK(saved(d.doc) == before);
    CHECK(undo_depth(d.doc) == 0);

    // The preview is a volume item the caller owns.
    clay_item* preview = nullptr;
    REQUIRE(clay_sdf_smooth_preview_item(tx, &preview) == CLAY_OK);
    REQUIRE(preview != nullptr);
    clay_item_destroy(preview);

    clay_sculpt_budget budget = budget_out();
    REQUIRE(clay_sdf_smooth_commit(tx, &budget) == CLAY_OK);
    CHECK(undo_depth(d.doc) == 1);  // one gesture, one step — not twenty-one
    CHECK(budget.over_budget == 0);
    CHECK(budget.consolidated == 0);
    CHECK(budget.item_count == 1);  // the layer is one baked item now

    clay_sdf_smooth_destroy(tx);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(saved(d.doc) == before);
}

TEST_CASE("c abi: destroying a Smooth transaction without committing is a cancel") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const std::vector<uint8_t> before = saved(d.doc);

    const clay_sculpt_policy policy = sculpt_policy(0.05f);
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
    REQUIRE(tx != nullptr);
    const clay_relax_params dab = relax_params(0.0f, 0.42f, 0.0f, 0.3f);
    REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);
    clay_sdf_smooth_destroy(tx);  // an error path that simply drops the handle

    CHECK(saved(d.doc) == before);
    CHECK(undo_depth(d.doc) == 0);
}

TEST_CASE("c abi: a Smooth transaction refuses what it cannot own") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    const clay_sculpt_policy good = sculpt_policy(0.05f);

    CHECK(clay_sdf_smooth_begin(nullptr, layer, &good, nullptr) == nullptr);
    CHECK(clay_sdf_smooth_begin(d.doc, layer, nullptr, nullptr) == nullptr);
    CHECK(clay_sdf_smooth_begin(d.doc, 9999, &good, nullptr) == nullptr);
    const clay_sculpt_policy no_cell = sculpt_policy(0.0f);
    CHECK(clay_sdf_smooth_begin(d.doc, layer, &no_cell, nullptr) == nullptr);

    // The struct_size prefix rule, exactly as every other descriptor states it.
    clay_sculpt_policy short_policy = good;
    short_policy.struct_size = 4;
    CHECK(clay_sdf_smooth_begin(d.doc, layer, &short_policy, nullptr) == nullptr);

    // A spent handle answers rather than crashes.
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &good, nullptr);
    REQUIRE(tx != nullptr);
    clay_sdf_smooth_cancel(tx);
    const clay_relax_params dab = relax_params(0, 0.42f, 0, 0.3f);
    CHECK(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_sdf_smooth_commit(tx, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_sdf_smooth_destroy(tx);
}

TEST_CASE("c abi: a Smooth commit refuses a layer that changed underneath it") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    const clay_sculpt_policy policy = sculpt_policy(0.05f);
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
    REQUIRE(tx != nullptr);
    const clay_relax_params dab = relax_params(0, 0.42f, 0, 0.3f);
    REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);

    clay_item_desc extra;
    std::memset(&extra, 0, sizeof extra);
    extra.struct_size = static_cast<uint32_t>(sizeof extra);
    extra.prim = CLAY_PRIM_SPHERE;
    extra.params[0] = 0.2f;
    extra.position[1] = 1.2f;
    clay_node_id added = 0;
    REQUIRE(clay_add_item(d.doc, layer, &extra, &added) == CLAY_OK);
    const std::vector<uint8_t> external = saved(d.doc);

    CHECK(clay_sdf_smooth_commit(tx, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(saved(d.doc) == external);  // the other edit survives untouched
    clay_sdf_smooth_destroy(tx);
}

TEST_CASE("c abi: a live Move previews without touching the document") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const std::vector<uint8_t> before = saved(d.doc);

    const float centre[3] = {0, 0, 0};
    const clay_move_params params = move_params(0.8f);
    clay_sdf_move_tx* tx = clay_sdf_move_begin(d.doc, layer, centre, &params, nullptr);
    REQUIRE(tx != nullptr);

    size_t count = 0;
    REQUIRE(clay_sdf_move_preview_nodes(tx, nullptr, 0, &count) == CLAY_OK);
    CHECK(count == 2);
    std::vector<clay_node_id> nodes(count);
    REQUIRE(clay_sdf_move_preview_nodes(tx, nodes.data(), nodes.size(), &count) == CLAY_OK);
    CHECK(clay_sdf_move_preview_nodes(tx, nodes.data(), 1, &count) == CLAY_ERROR_BUFFER_TOO_SMALL);

    for (int i = 1; i <= 50; ++i) {
        const float total[3] = {0.0f, 0.008f * static_cast<float>(i), 0.0f};
        clay_sculpt_dirty dirty = dirty_out();
        REQUIRE(clay_sdf_move_update(tx, total, &dirty) == CLAY_OK);
        CHECK(dirty.changed == 1);
        CHECK(dirty.has_bounds == 1);
        CHECK(dirty.touched_bricks == 2);
    }
    CHECK(saved(d.doc) == before);  // fifty frames, zero persistent commands
    CHECK(undo_depth(d.doc) == 0);

    // The grab the last update resolved, in the node's own frame: what
    // clay_item_add_deformer(CLAY_DEFORM_GRAB, ...) takes.
    float grab_centre[3] = {0, 0, 0};
    float grab_disp[3] = {0, 0, 0};
    float grab_radius = 0.0f;
    int32_t ease = -1, front_only = -1;
    size_t grabs = 0;
    REQUIRE(clay_sdf_move_preview_grab_count(tx, nodes[0], &grabs) == CLAY_OK);
    CHECK(grabs == 1);  // no symmetry: the ball alone
    REQUIRE(clay_sdf_move_preview_grab(tx, nodes[0], 0, grab_centre, &grab_radius, grab_disp,
                                       &ease, &front_only) == CLAY_OK);
    CHECK(grab_radius == doctest::Approx(0.8f));
    CHECK(grab_disp[1] == doctest::Approx(0.4f));  // the TOTAL, not the last increment
    CHECK(ease == 0);
    CHECK(front_only == 0);
    CHECK(clay_sdf_move_preview_grab(tx, nodes[0], 1, nullptr, nullptr, nullptr, nullptr,
                                     nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_sdf_move_preview_grab(tx, 9999, 0, nullptr, nullptr, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_sdf_move_preview_grab_count(tx, 9999, &grabs) == CLAY_ERROR_NOT_FOUND);

    clay_sculpt_budget budget = budget_out();
    REQUIRE(clay_sdf_move_commit(tx, &budget) == CLAY_OK);
    CHECK(undo_depth(d.doc) == 1);  // one drag, one step
    CHECK(budget.longest_deformer_chain == 1);
    CHECK(budget.over_budget == 0);
    clay_sdf_move_destroy(tx);

    const std::vector<uint8_t> after = saved(d.doc);
    CHECK(after != before);
    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(saved(d.doc) == before);
}

TEST_CASE("c abi: a Move transaction refuses what is not a drag") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    const float centre[3] = {0, 0, 0};
    const clay_move_params good = move_params(0.8f);

    CHECK(clay_sdf_move_begin(nullptr, layer, centre, &good, nullptr) == nullptr);
    CHECK(clay_sdf_move_begin(d.doc, layer, nullptr, &good, nullptr) == nullptr);
    CHECK(clay_sdf_move_begin(d.doc, layer, centre, nullptr, nullptr) == nullptr);
    CHECK(clay_sdf_move_begin(d.doc, 9999, centre, &good, nullptr) == nullptr);
    const clay_move_params no_radius = move_params(0.0f);
    CHECK(clay_sdf_move_begin(d.doc, layer, centre, &no_radius, nullptr) == nullptr);
}

TEST_CASE("c abi: an authorised sculpt policy bounds a repeated Move") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);

    clay_sculpt_policy policy = sculpt_policy(0.05f);
    policy.max_deformer_chain = 1;      // crossed by the second stroke
    policy.allow_consolidation = 1;
    const clay_move_params params = move_params(0.8f);

    for (int stroke = 0; stroke < 2; ++stroke) {
        const float centre[3] = {0.0f, 0.05f * static_cast<float>(stroke), 0.0f};
        clay_sdf_move_tx* tx = clay_sdf_move_begin(d.doc, layer, centre, &params, &policy);
        REQUIRE(tx != nullptr);
        const float total[3] = {0.0f, 0.1f, 0.0f};
        REQUIRE(clay_sdf_move_update(tx, total, nullptr) == CLAY_OK);
        clay_sculpt_budget budget = budget_out();
        REQUIRE(clay_sdf_move_commit(tx, &budget) == CLAY_OK);
        if (stroke == 1) {
            CHECK(budget.over_budget == 1);
            CHECK(budget.consolidated == 1);
            // A volume carries no deformers, so the chain degradation is gone.
            CHECK(budget.longest_deformer_chain == 0);
        }
        clay_sdf_move_destroy(tx);
    }

    int32_t consolidated = 0;
    REQUIRE(clay_layer_consolidation_state(d.doc, layer, &consolidated, nullptr) == CLAY_OK);
    CHECK(consolidated == 1);
    // The stroke AND the consolidation it triggered are ONE thing to undo.
    CHECK(undo_depth(d.doc) == 2);
}

// -- the incremental preview delta (add-sdf-prefix-cache) ----------------------

namespace {

clay_sdf_preview_delta_info delta_info_out() {
    clay_sdf_preview_delta_info i;
    std::memset(&i, 0, sizeof i);
    i.struct_size = static_cast<uint32_t>(sizeof i);
    return i;
}

}  // namespace

TEST_CASE("c abi: a preview delta carries only the bricks that changed") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    const clay_sculpt_policy policy = sculpt_policy(0.05f);
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
    REQUIRE(tx != nullptr);

    // Nothing has happened yet, so there is nothing to take and no generation.
    clay_sdf_preview_delta_info info = delta_info_out();
    REQUIRE(clay_sdf_smooth_preview_delta_info(tx, &info) == CLAY_OK);
    CHECK(info.brick_count == 0);
    CHECK(info.generation == 0);
    CHECK(info.has_bounds == 0);

    const clay_relax_params dab = relax_params(0.0f, 0.42f, 0.0f, 0.25f);
    REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);

    REQUIRE(clay_sdf_smooth_preview_delta_info(tx, &info) == CLAY_OK);
    CHECK(info.brick_count > 0);
    CHECK(info.generation == 1);
    CHECK(info.has_bounds == 1);
    CHECK(info.sample_floats == info.brick_count * 729);  // 9^3, halo included
    CHECK(info.bounds_min[0] < info.bounds_max[0]);

    // A short buffer takes NOTHING and says how far it fell short, so the
    // caller can grow and ask again without having stranded a brick.
    std::vector<clay_sdf_preview_brick> bricks(1);
    std::vector<float> samples(729);
    uint64_t got_bricks = 0, got_samples = 0;
    CHECK(clay_sdf_smooth_preview_delta_take(tx, bricks.data(), 1, samples.data(), 729,
                                             &got_bricks, &got_samples) ==
          CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(got_bricks == info.brick_count);
    CHECK(got_samples == info.sample_floats);

    clay_sdf_preview_delta_info still = delta_info_out();
    REQUIRE(clay_sdf_smooth_preview_delta_info(tx, &still) == CLAY_OK);
    CHECK(still.brick_count == info.brick_count);  // nothing was taken

    bricks.resize(static_cast<std::size_t>(info.brick_count));
    samples.resize(static_cast<std::size_t>(info.sample_floats));
    REQUIRE(clay_sdf_smooth_preview_delta_take(tx, bricks.data(), bricks.size(), samples.data(),
                                               samples.size(), &got_bricks,
                                               &got_samples) == CLAY_OK);
    CHECK(got_bricks == info.brick_count);
    CHECK(got_samples == info.sample_floats);
    for (uint64_t i = 0; i < got_bricks; ++i) {
        CHECK(bricks[i].sample_dim == 9);
        CHECK(bricks[i].spacing == doctest::Approx(0.05f));
        CHECK(bricks[i].sample_offset == i * 729);
    }

    // Taking clears it; the generation stays, because it names what the caller
    // now holds rather than what is waiting.
    clay_sdf_preview_delta_info after = delta_info_out();
    REQUIRE(clay_sdf_smooth_preview_delta_info(tx, &after) == CLAY_OK);
    CHECK(after.brick_count == 0);
    CHECK(after.generation == 1);

    clay_sdf_smooth_destroy(tx);
}

TEST_CASE("c abi: the delta accumulates across frames and dedups by brick") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    const clay_sculpt_policy policy = sculpt_policy(0.05f);
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
    REQUIRE(tx != nullptr);
    const clay_relax_params dab = relax_params(0.0f, 0.42f, 0.0f, 0.25f);

    REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);
    clay_sdf_preview_delta_info first = delta_info_out();
    REQUIRE(clay_sdf_smooth_preview_delta_info(tx, &first) == CLAY_OK);

    // The SAME dab again: it materializes nothing new and moves the same
    // bricks, so a host that skipped the first frame is told about them once.
    REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);
    clay_sdf_preview_delta_info second = delta_info_out();
    REQUIRE(clay_sdf_smooth_preview_delta_info(tx, &second) == CLAY_OK);
    CHECK(second.brick_count == first.brick_count);
    CHECK(second.generation == 2);  // the preview did move, twice

    clay_sdf_smooth_destroy(tx);
}

TEST_CASE("c abi: delta payload follows the brush, not the model") {
    // 11.2: the whole point. The same tiny dab on a layer with far more in it
    // must hand over the same number of bricks.
    uint64_t small_bricks = 0, large_bricks = 0;
    for (int extra : {0, 300}) {
        CDoc d;
        const clay_layer_id layer = blended_form(d.doc);
        for (int i = 0; i < extra; ++i) {
            clay_item_desc it;
            std::memset(&it, 0, sizeof it);
            it.struct_size = static_cast<uint32_t>(sizeof it);
            it.prim = CLAY_PRIM_SPHERE;
            it.params[0] = 0.05f;
            it.position[0] = 20.0f + 0.2f * static_cast<float>(i);
            clay_node_id node = 0;
            REQUIRE(clay_add_item(d.doc, layer, &it, &node) == CLAY_OK);
        }
        const clay_sculpt_policy policy = sculpt_policy(0.05f);
        clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
        REQUIRE(tx != nullptr);
        const clay_relax_params dab = relax_params(0.0f, 0.42f, 0.0f, 0.25f);
        REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);
        clay_sdf_preview_delta_info info = delta_info_out();
        REQUIRE(clay_sdf_smooth_preview_delta_info(tx, &info) == CLAY_OK);
        (extra == 0 ? small_bricks : large_bricks) = info.brick_count;
        clay_sdf_smooth_destroy(tx);
    }
    CHECK(small_bricks > 0);
    CHECK(small_bricks == large_bricks);
}

TEST_CASE("c abi: the delta refuses a spent transaction rather than dangling") {
    CDoc d;
    const clay_layer_id layer = blended_form(d.doc);
    const clay_sculpt_policy policy = sculpt_policy(0.05f);
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
    REQUIRE(tx != nullptr);
    const clay_relax_params dab = relax_params(0.0f, 0.42f, 0.0f, 0.25f);
    REQUIRE(clay_sdf_smooth_update(tx, &dab, nullptr, nullptr) == CLAY_OK);

    clay_sdf_smooth_cancel(tx);
    clay_sdf_preview_delta_info info = delta_info_out();
    uint64_t a = 0, b = 0;
    CHECK(clay_sdf_smooth_preview_delta_info(tx, &info) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_sdf_smooth_preview_delta_take(tx, nullptr, 0, nullptr, 0, &a, &b) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_sdf_smooth_preview_delta_info(nullptr, &info) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_sdf_smooth_destroy(tx);
}
