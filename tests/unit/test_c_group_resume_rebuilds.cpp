#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "clay.h"

// A stroke into a group must rebuild each brick ONCE, not repeatedly (#508).
//
// `run_resume_task` compiles a suffix onto the brick's seed; where
// `compile_layer_suffix` refuses, the brick takes a full walk of the active
// half instead -- what the whole DOCUMENT costs rather than what the dab does.
// That refusal is meant to happen once per brick per stroke, to give a brick
// its first stack. It was happening 5.56 times per brick.
//
// THE CAUSE WAS A PARTIAL WRITE, and the comment above the store said it could
// not happen: "the stack, the depth and the frames go together or not at all --
// a reader takes all three or none". The reader takes FOUR things. That path
// wrote `stack`, `stack_levels` and `frames` and left `layer_have_acc` at
// whatever the entry already held, while validating against the INCOMING one.
// So a write passed its own check and the matching read failed:
//
//     write:  checkpoint_stack_levels(*frames,    layer_have_acc)  == stack_levels
//     read:   checkpoint_stack_levels(e->frames, e->layer_have_acc) == e->stack_levels
//
// GATED ON A COUNT, not a duration, because a count is the same on every
// machine and this one has an exact expected value: the number of distinct
// bricks the stroke touches. A wall-clock bound would have to be loose enough
// to catch nothing.
//
// The document size matters and is not arbitrary. The engine already records a
// BAND where the cull pad steps mid-stroke -- "clean at 20, 50 and 80 stamps,
// wrong at 100 and 120, clean again from 200" -- and 100 stamps is inside it.
// Outside the band the fast path was already firing, so a fixture at 1000
// stamps passes with the fix reverted and proves nothing.

namespace {

constexpr int kStamps = 100;  // inside the band; see above
constexpr int kDabs = 24;
constexpr float kVoxel = 0.05f;

void stamp_position(int i, float* p) {
    const double mx = 0.4142135624, my = 0.7320508076, mz = 0.2360679775;
    p[0] = float(std::fmod(double(i) * mx, 1.0)) * 1.6f - 0.8f;
    p[1] = float(std::fmod(double(i) * my, 1.0)) * 1.6f - 0.8f;
    p[2] = float(std::fmod(double(i) * mz, 1.0)) * 1.6f - 0.8f;
}
void dab_position(int k, float* p) {
    p[0] = -0.30f + 0.025f * float(k);
    p[1] = 0.10f;
    p[2] = 0.05f;
}

clay_node_id add_sphere(clay_document* doc, clay_layer_id layer, const float* pos,
                        const clay_node_id* group) {
    float params[1] = {0.12f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_position(item, pos) == CLAY_OK);
    clay_node_id node = 0;
    const clay_result r =
        group ? clay_layer_add_item_in_group(doc, layer, *group, -1, item, &node)
              : clay_layer_add_item(doc, layer, item, &node);
    clay_item_destroy(item);
    REQUIRE(r == CLAY_OK);
    return node;
}

struct Run {
    std::uint64_t rebuilds = 0;
    std::uint64_t stack_shape_misses = 0;
    std::size_t distinct_bricks = 0;
};

Run stroke_into_group() {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "bench", &layer) == CLAY_OK);

    float p[3];
    for (int i = 0; i < kStamps; ++i) {
        stamp_position(i, p);
        add_sphere(doc, layer, p, nullptr);
    }
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(doc, layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC, 0.05f,
                                 0.0f, &group) == CLAY_OK);

    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.voxel_size = kVoxel;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);

    const std::size_t per = std::size_t(cfg.dim) * cfg.dim * cfg.dim;
    std::vector<clay_brick_request> reqs(8192);
    std::vector<float> vals(reqs.size() * per);
    std::set<std::array<int, 3>> distinct;

    auto pump = [&](bool record) {
        for (;;) {
            std::size_t n = reqs.size(), remaining = 0;
            if (clay_brick_cache_take_dirty(cache, reqs.data(), &n, &remaining) != CLAY_OK ||
                n == 0)
                return;
            clay_brick_cache_eval_requests(doc, "cpu", reqs.data(), n, vals.data(), n * per,
                                           nullptr, 0);
            clay_brick_cache_submit(cache, reqs.data(), n, vals.data(), n * per, nullptr, 0,
                                    nullptr, nullptr);
            if (record)
                for (std::size_t i = 0; i < n; ++i)
                    distinct.insert({reqs[i].key[0], reqs[i].key[1], reqs[i].key[2]});
            if (remaining == 0) return;
        }
    };

    clay_brick_cache_mark_dirty_layer(cache, doc, layer);
    pump(false);  // the first fill is a load cost, not a stroke cost

    clay_resume_stats before;
    std::memset(&before, 0, sizeof before);
    before.struct_size = sizeof before;
    REQUIRE(clay_document_resume_stats(doc, &before) == CLAY_OK);

    for (int k = 0; k < kDabs; ++k) {
        dab_position(k, p);
        const clay_node_id node = add_sphere(doc, layer, p, &group);
        clay_node_id one[1] = {node};
        REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc, layer, one, 1, nullptr) == CLAY_OK);
        pump(true);
    }

    clay_resume_stats after;
    std::memset(&after, 0, sizeof after);
    after.struct_size = sizeof after;
    REQUIRE(clay_document_resume_stats(doc, &after) == CLAY_OK);

    Run out;
    out.rebuilds = after.resume_full_rebuilds - before.resume_full_rebuilds;
    out.stack_shape_misses = after.seed_miss_stack_shape - before.seed_miss_stack_shape;
    out.distinct_bricks = distinct.size();

    clay_brick_cache_destroy(cache);
    clay_document_destroy(doc);
    return out;
}

}  // namespace

TEST_CASE("a stroke into a group rebuilds each brick once, not repeatedly") {
    const Run r = stroke_into_group();

    // The fixture must reach the path at all: a stroke that touched no brick,
    // or rebuilt nothing, would pass every assertion below while measuring
    // nothing.
    REQUIRE(r.distinct_bricks > 0);
    REQUIRE(r.rebuilds > 0);

    // A brick's FIRST touch has no stack and must walk once to acquire one.
    // Every touch after that continues from the seed. So the rebuild count is
    // the number of distinct bricks -- not a fraction of the refills, which is
    // what it was: 267 rebuilds over 48 bricks, 5.56 each.
    CHECK(r.rebuilds == r.distinct_bricks);

    // And the reason, asserted separately so a regression says WHICH half broke:
    // a stored stack must never be turned down for disagreeing with itself.
    CHECK(r.stack_shape_misses == 0);
}
