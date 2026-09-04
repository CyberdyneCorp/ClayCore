// The layer-extent memo across a drag (c-abi spec, issue #451).
//
// #319 gave an intersect a finite bound and made computing it a walk of every
// visible node. #454 stopped that walk repeating per intersect within one
// query, and named what it left: `apply_edit` takes `command_influence_bound`
// on BOTH sides of every command, so a drag paid two full layer walks a frame
// -- 0.0353 ms against 0.0003 for the same drag with a subtracting operand.
//
// COUNTED, NOT TIMED, and this file is why that matters rather than being a
// stylistic preference. The first attempt keyed the memo on the ABI's
// `revision`, which advances at the END of an edit -- so both calls saw the
// same key and the second was answered with the FIRST's geometry: a bound too
// small, which is under-invalidation and shows up as stale bricks rather than
// as an error. It measured a clean 2x. Every existing test passed. What caught
// it was this counter reading "one walk, zero reuses" a frame, which is only
// possible if both calls shared a key.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "clay.h"

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

clay_extent_stats stats_of(const clay_document* doc) {
    clay_extent_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = static_cast<uint32_t>(sizeof s);
    REQUIRE(clay_document_extent_stats(doc, &s) == CLAY_OK);
    return s;
}

clay_node_id build(clay_document* doc, clay_layer_id* out_layer, int32_t op) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_node_id n = 0;
    for (int i = 0; i < 40; ++i) {
        clay_item_desc s;
        std::memset(&s, 0, sizeof s);
        s.struct_size = static_cast<uint32_t>(sizeof s);
        s.prim = CLAY_PRIM_SPHERE;
        s.params[0] = 0.2f;
        s.op = CLAY_OP_ADD;
        s.position[0] = 0.05f * static_cast<float>(i);
        REQUIRE(clay_add_item(doc, layer, &s, &n) == CLAY_OK);
    }
    clay_item_desc c;
    std::memset(&c, 0, sizeof c);
    c.struct_size = static_cast<uint32_t>(sizeof c);
    c.prim = CLAY_PRIM_BOX;
    c.params[0] = c.params[1] = c.params[2] = 0.4f;
    c.op = op;
    clay_node_id drag = 0;
    REQUIRE(clay_add_item(doc, layer, &c, &drag) == CLAY_OK);
    *out_layer = layer;
    return drag;
}

void drag(clay_document* doc, clay_layer_id layer, clay_node_id node, int frames) {
    for (int i = 0; i < frames; ++i) {
        const float pos[3] = {0.01f * static_cast<float>(i), 0.0f, 0.0f};
        const float axis[3] = {0, 1, 0};
        REQUIRE(clay_layer_set_transform(doc, layer, node, pos, axis, 0.0f, 1.0f) == CLAY_OK);
    }
}

}  // namespace

TEST_CASE("c abi: a drag walks its layer once a frame, not twice") {
    CDoc d;
    clay_layer_id layer = 0;
    const clay_node_id node = build(d.doc, &layer, CLAY_OP_INTERSECT);

    const clay_extent_stats before = stats_of(d.doc);
    const int frames = 20;
    drag(d.doc, layer, node, frames);
    const clay_extent_stats after = stats_of(d.doc);

    const uint64_t walks = after.walks - before.walks;
    const uint64_t reuses = after.reuses - before.reuses;
    MESSAGE("over " << frames << " frames: " << walks << " walks, " << reuses << " reused");

    // Each edit takes the bound on both sides, so two per frame is what it
    // costs with no memo. One of the two is answered from the previous frame's,
    // so both counts come out at about one a frame.
    CHECK(walks <= static_cast<uint64_t>(frames) + 1);
    CHECK(reuses >= static_cast<uint64_t>(frames) - 1);
}

TEST_CASE("c abi: a layer with no intersect never walks at all") {
    // The extent exists only to bound an intersect, so a subtracting drag on
    // the same fixture must not pay for one. This is the control the issue
    // reports as flat, and it says the memo is unreached rather than merely
    // fast.
    CDoc d;
    clay_layer_id layer = 0;
    const clay_node_id node = build(d.doc, &layer, CLAY_OP_SUBTRACT);
    drag(d.doc, layer, node, 20);
    const clay_extent_stats s = stats_of(d.doc);
    CHECK(s.walks == 0);
    CHECK(s.reuses == 0);
}

TEST_CASE("c abi: the memo never answers with a document that has moved") {
    // THE TRAP THE FIRST ATTEMPT FELL INTO, as a test rather than a comment.
    // The bound an edit reports must reflect the document AFTER it, so growing
    // the layer must grow the intersect's bound on the very next query -- and
    // an edit that follows must not be answered from a memo taken before it.
    CDoc d;
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    clay_layer_id layer = 0;
    const clay_node_id node = build(d.doc, &layer, CLAY_OP_INTERSECT);

    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    int32_t has = 0, infinite = 0;
    REQUIRE(clay_layer_node_influence_bound(d.doc, layer, node, lo, hi, &has, &infinite) ==
            CLAY_OK);
    REQUIRE(has != 0);
    REQUIRE(infinite == 0);

    clay_item_desc far_item;
    std::memset(&far_item, 0, sizeof far_item);
    far_item.struct_size = static_cast<uint32_t>(sizeof far_item);
    far_item.prim = CLAY_PRIM_SPHERE;
    far_item.params[0] = 0.5f;
    far_item.op = CLAY_OP_ADD;
    far_item.position[0] = 20.0f;
    clay_node_id added = 0;
    REQUIRE(clay_add_item(d.doc, layer, &far_item, &added) == CLAY_OK);

    float lo2[3] = {0, 0, 0}, hi2[3] = {0, 0, 0};
    REQUIRE(clay_layer_node_influence_bound(d.doc, layer, node, lo2, hi2, &has, &infinite) ==
            CLAY_OK);
    CHECK(hi2[0] > hi[0] + 10.0f);

    // And an UNDO puts it back. Undo does not go through the command funnel the
    // ABI uses, but it does go through `scene::apply`, which is where
    // `content_serial` advances -- so a memo that only knew about the funnel
    // would answer this one from before the undo.
    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    REQUIRE(undone != 0);
    float lo3[3] = {0, 0, 0}, hi3[3] = {0, 0, 0};
    REQUIRE(clay_layer_node_influence_bound(d.doc, layer, node, lo3, hi3, &has, &infinite) ==
            CLAY_OK);
    CHECK(hi3[0] == doctest::Approx(hi[0]));
}
