#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// What a host must dirty when it edits a node INSIDE a blended group
// (issue #497, from the 0.96.0 device gate).
//
// `clay_brick_cache_mark_dirty_nodes` and `clay_layer_node_influence_bound`
// answer from one body, so this tests the query and then checks the dirty call
// agrees. The claim is that a node inside a group reaches PAST its own box by
// that group's blend support: a group's value is a combine over its children,
// and a blend's support is exactly how far changing one operand can move the
// result.
//
// WHY THIS IS A BEHAVIOURAL TEST AND NOT A BOUNDS UNIT TEST. There is already
// a C++ test that node_reach_bound dilates (test_node_reach_bound.cpp). What
// was missing, and what the device gate caught as a 2.30x "regression", is
// that the DIRTY path uses it: before it did, an in-group stroke marked the
// un-dilated box and left bricks holding pre-dab values. Measured on the
// 100-stamp fixture the gate uses, 17 of 216 bricks were stale, and the same
// stroke with its dabs at the layer root left none. That is the shape a
// sculptor sees as a seam that does not update, and no unit test on the bound
// alone would have failed while the dirty call read a different one.
//
// The cost is real and is not a defect: marking what the edit reaches quadruples
// the bricks an in-group dab refills (232 -> 924 over a 24-dab stroke). The
// baseline carries that with its reason.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_node_id add_sphere(Doc& doc, float radius, const float pos[3], const clay_node_id* group) {
    float params[1] = {radius};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_position(item, pos) == CLAY_OK);
    clay_node_id node = 0;
    const clay_result r =
        group ? clay_layer_add_item_in_group(doc.d, doc.layer, *group, -1, item, &node)
              : clay_layer_add_item(doc.d, doc.layer, item, &node);
    clay_item_destroy(item);
    REQUIRE(r == CLAY_OK);
    return node;
}

struct Box {
    float lo[3];
    float hi[3];
    float span(int axis) const { return hi[axis] - lo[axis]; }
};

Box bound_of(const Doc& doc, clay_node_id node) {
    Box b{};
    std::int32_t has = 0, infinite = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, node, b.lo, b.hi, &has, &infinite) ==
            CLAY_OK);
    REQUIRE(has == 1);
    REQUIRE(infinite == 0);
    return b;
}

}  // namespace

TEST_CASE("a dab inside a blended group dirties past its own box") {
    // The two documents differ in ONE thing: where the dab is parented. Same
    // radius, same position, same layer — so a difference in the reported box
    // is the group's blend support and nothing else.
    const float pos[3] = {0.10f, 0.10f, 0.05f};
    const float radius = 0.12f;
    const float blend_k = 0.05f;

    Doc rooted;
    const clay_node_id at_root = add_sphere(rooted, radius, pos, nullptr);
    const Box root_box = bound_of(rooted, at_root);

    Doc grouped;
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(grouped.d, grouped.layer, 0, -1, CLAY_OP_ADD,
                                 CLAY_BLEND_QUADRATIC, blend_k, 0.0f, &group) == CLAY_OK);
    // A sibling, so the group is a blend over two operands rather than a
    // pass-through of one — which is the case a stroke is in from its second
    // dab onward.
    const float sibling[3] = {0.10f - 2.0f * radius, 0.10f, 0.05f};
    add_sphere(grouped, radius, sibling, &group);
    const clay_node_id in_group = add_sphere(grouped, radius, pos, &group);
    const Box group_box = bound_of(grouped, in_group);

    // Strictly larger on every axis, and by the same amount on each: a blend
    // support is a radius, not a direction.
    for (int axis = 0; axis < 3; ++axis) {
        CAPTURE(axis);
        CHECK(group_box.lo[axis] < root_box.lo[axis]);
        CHECK(group_box.hi[axis] > root_box.hi[axis]);
    }
    const float grew = group_box.span(0) - root_box.span(0);
    CHECK(grew > 0.0f);
    for (int axis = 1; axis < 3; ++axis) {
        CAPTURE(axis);
        CHECK(group_box.span(axis) - root_box.span(axis) == doctest::Approx(grew).epsilon(1e-4));
    }

    // And it is the BLEND's reach, not an arbitrary pad: a hard group with no
    // blend adds nothing, which is what says the dilation above is the support
    // rather than a constant somebody chose.
    Doc hard;
    clay_node_id hard_group = 0;
    REQUIRE(clay_layer_add_group(hard.d, hard.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f,
                                 0.0f, &hard_group) == CLAY_OK);
    add_sphere(hard, radius, sibling, &hard_group);
    const clay_node_id in_hard = add_sphere(hard, radius, pos, &hard_group);
    const Box hard_box = bound_of(hard, in_hard);
    for (int axis = 0; axis < 3; ++axis) {
        CAPTURE(axis);
        CHECK(hard_box.span(axis) == doctest::Approx(root_box.span(axis)).epsilon(1e-4));
    }
}

TEST_CASE("the dirty call marks the same reach the query reports") {
    // The gate's failure was not that the bound was wrong — it was that the
    // dirty path read a different one. This is the half that catches that: the
    // number of bricks marked for an in-group dab must exceed the number for
    // the identical dab at the root, because the box does.
    const float pos[3] = {0.10f, 0.10f, 0.05f};
    const float radius = 0.12f;

    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.voxel_size = 0.05f;

    auto marked_bricks = [&](bool in_group) {
        Doc doc;
        clay_node_id group = 0;
        if (in_group) {
            REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD,
                                         CLAY_BLEND_QUADRATIC, 0.05f, 0.0f, &group) == CLAY_OK);
            const float sibling[3] = {0.10f - 2.0f * radius, 0.10f, 0.05f};
            add_sphere(doc, radius, sibling, &group);
        }
        clay_brick_cache* cache = clay_brick_cache_create(&cfg);
        REQUIRE(cache != nullptr);
        const clay_node_id node = add_sphere(doc, radius, pos, in_group ? &group : nullptr);
        clay_node_id one[1] = {node};
        REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.layer, one, 1, nullptr) ==
                CLAY_OK);
        clay_brick_stats stats;
        std::memset(&stats, 0, sizeof stats);
        stats.struct_size = sizeof stats;
        REQUIRE(clay_brick_cache_stats(cache, &stats) == CLAY_OK);
        const std::uint64_t dirty = stats.dirty_bricks;
        clay_brick_cache_destroy(cache);
        return dirty;
    };

    const std::uint64_t at_root = marked_bricks(false);
    const std::uint64_t in_group = marked_bricks(true);
    CHECK(at_root > 0);  // the fixture reaches the path at all
    CHECK(in_group > at_root);
}
