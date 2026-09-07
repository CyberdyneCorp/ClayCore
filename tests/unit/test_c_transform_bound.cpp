#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// `clay_layer_set_transform_bound` across the C boundary (c-abi spec, issue
// #471): the edit `clay_layer_set_transform` applies, plus the box it changed.
//
// The two halves are tested separately and both matter. That the reported box
// is SMALL is worth nothing unless the edit it reports on is the same edit —
// one command, one undo step, the same document afterwards — and that the box
// is small for an intersect is worth nothing unless the GENERIC query beside it
// still reports the layer, because everything else in the engine reads that one.

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

clay_node_id add(Doc& doc, int32_t prim, const float params[7], const float pos[3], int32_t op) {
    clay_item_desc it;
    std::memset(&it, 0, sizeof it);
    it.struct_size = sizeof(it);
    it.prim = prim;
    for (int i = 0; i < 7; ++i) it.params[i] = params[i];
    for (int i = 0; i < 3; ++i) it.position[i] = pos[i];
    it.rotation[3] = 1.0f;
    it.scale = 1.0f;
    it.op = op;
    clay_node_id id = 0;
    REQUIRE(clay_add_item(doc.d, doc.layer, &it, &id) == CLAY_OK);
    return id;
}

// A form worth cutting: a sphere and four lumps, spanning about [-1.4, 1.4].
void add_body(Doc& doc) {
    const float r[7] = {0.9f, 0, 0, 0, 0, 0, 0};
    const float o[3] = {0, 0, 0};
    add(doc, CLAY_PRIM_SPHERE, r, o, CLAY_OP_ADD);
    for (int i = 0; i < 4; ++i) {
        const float a = 1.5707963f * static_cast<float>(i);
        const float p[3] = {std::cos(a) * 0.8f, 0.3f, std::sin(a) * 0.8f};
        const float s[7] = {0.45f, 0, 0, 0, 0, 0, 0};
        add(doc, CLAY_PRIM_SPHERE, s, p, CLAY_OP_ADD);
    }
}

struct Box {
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    std::int32_t has = 0;
    std::int32_t infinite = 0;
    double volume() const {
        if (!has || infinite) return 0.0;
        return static_cast<double>(max[0] - min[0]) * (max[1] - min[1]) * (max[2] - min[2]);
    }
};

Box move(Doc& doc, clay_node_id node, float x) {
    Box b;
    const float pos[3] = {x, 0.2f, 0};
    const float axis[3] = {0, 1, 0};
    REQUIRE(clay_layer_set_transform_bound(doc.d, doc.layer, node, pos, axis, 0.0f, 1.0f, b.min,
                                           b.max, &b.has, &b.infinite) == CLAY_OK);
    return b;
}

Box influence(const Doc& doc, clay_node_id node) {
    Box b;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, node, b.min, b.max, &b.has,
                                            &b.infinite) == CLAY_OK);
    return b;
}

const float kCutter[7] = {0.3f, 0.8f, 0, 0, 0, 0, 0};

}  // namespace

TEST_CASE("c abi: a moved intersect reports the box it changed, not its layer") {
    Doc doc;
    add_body(doc);
    const float start[3] = {-0.6f, 0.2f, 0};
    const clay_node_id cutter =
        add(doc, CLAY_PRIM_CAPPED_CYLINDER, kCutter, start, CLAY_OP_INTERSECT);

    const Box reach = move(doc, cutter, 0.5f);
    REQUIRE(reach.has == 1);
    REQUIRE(reach.infinite == 0);

    // THE GENERIC QUERY IS UNCHANGED, which is the invariant every other
    // consumer reads: an arbitrary edit to an intersect really can reach the
    // whole layer, and this change does not say otherwise.
    const Box generic = influence(doc, cutter);
    REQUIRE(generic.has == 1);
    REQUIRE(generic.infinite == 0);
    CHECK(generic.volume() > 4.0 * reach.volume());

    // The reported box holds the operand where it went, with room for the
    // blend and pad dilations.
    CHECK(reach.min[0] <= -0.6f - 0.3f);
    CHECK(reach.max[0] >= 0.5f + 0.3f);
    // ... and does not hold the whole form.
    CHECK(reach.min[1] > -1.4f);
}

TEST_CASE("c abi: the transform bound is the same edit as the plain setter") {
    // Same document, same move, one through each entry point: the documents
    // must agree afterwards, and each must be one undo step.
    Doc a, b;
    add_body(a);
    add_body(b);
    const float start[3] = {-0.6f, 0.2f, 0};
    const clay_node_id ca = add(a, CLAY_PRIM_CAPPED_CYLINDER, kCutter, start, CLAY_OP_INTERSECT);
    const clay_node_id cb = add(b, CLAY_PRIM_CAPPED_CYLINDER, kCutter, start, CLAY_OP_INTERSECT);
    REQUIRE(ca == cb);
    REQUIRE(clay_document_enable_undo(a.d) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(b.d) == CLAY_OK);

    const float pos[3] = {0.5f, 0.2f, 0};
    const float axis[3] = {0, 1, 0};
    REQUIRE(clay_layer_set_transform(a.d, a.layer, ca, pos, axis, 0.0f, 1.0f) == CLAY_OK);
    REQUIRE(clay_layer_set_transform_bound(b.d, b.layer, cb, pos, axis, 0.0f, 1.0f, nullptr,
                                           nullptr, nullptr, nullptr) == CLAY_OK);

    const float pts[6] = {0.4f, 0.2f, 0.0f, -0.9f, 0.0f, 0.0f};
    float da[2] = {0, 0}, db[2] = {0, 0};
    REQUIRE(clay_eval_points(a.d, nullptr, pts, 2, da, nullptr) == CLAY_OK);
    REQUIRE(clay_eval_points(b.d, nullptr, pts, 2, db, nullptr) == CLAY_OK);
    CHECK(da[0] == db[0]);
    CHECK(da[1] == db[1]);

    std::size_t depth_a = 0, depth_b = 0;
    REQUIRE(clay_document_undo_state(a.d, nullptr, &depth_a, nullptr) == CLAY_OK);
    REQUIRE(clay_document_undo_state(b.d, nullptr, &depth_b, nullptr) == CLAY_OK);
    CHECK(depth_a == depth_b);
    // Undo was enabled after the document was built, so the move is the only
    // step either of them has.
    CHECK(depth_b == 1);
}

TEST_CASE("c abi: the transform bound falls back where it cannot prove a delta") {
    SUBCASE("a subtracting operand keeps its own geometry bound") {
        Doc doc;
        add_body(doc);
        const float start[3] = {-0.6f, 0.2f, 0};
        const clay_node_id cutter =
            add(doc, CLAY_PRIM_CAPPED_CYLINDER, kCutter, start, CLAY_OP_SUBTRACT);
        const Box reach = move(doc, cutter, 0.5f);
        const Box generic = influence(doc, cutter);
        REQUIRE(reach.has == 1);
        REQUIRE(generic.has == 1);
        // The influence bound of a local op IS this box, so the fallback and
        // the delta would be the same answer and the delta stands aside.
        CHECK(reach.max[0] == doctest::Approx(generic.max[0]));
    }
    SUBCASE("an unbounded primitive says so") {
        Doc doc;
        add_body(doc);
        const float plane[7] = {0, 1, 0, 0.2f, 0, 0, 0};
        const float at[3] = {0, 0, 0};
        const clay_node_id cutter = add(doc, CLAY_PRIM_PLANE, plane, at, CLAY_OP_INTERSECT);
        const Box reach = move(doc, cutter, 0.3f);
        CHECK(reach.has == 1);
        CHECK(reach.infinite == 1);
    }
    SUBCASE("a group is refused, as the plain setter refuses it") {
        Doc doc;
        add_body(doc);
        clay_node_id group = 0;
        REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f,
                                     0.0f, &group) == CLAY_OK);
        const float pos[3] = {0.5f, 0, 0};
        const float axis[3] = {0, 1, 0};
        Box b;
        CHECK(clay_layer_set_transform_bound(doc.d, doc.layer, group, pos, axis, 0.0f, 1.0f,
                                             b.min, b.max, &b.has, &b.infinite) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a node the layer does not hold is not found") {
        Doc doc;
        add_body(doc);
        const float pos[3] = {0.5f, 0, 0};
        const float axis[3] = {0, 1, 0};
        Box b;
        CHECK(clay_layer_set_transform_bound(doc.d, doc.layer, 9999, pos, axis, 0.0f, 1.0f,
                                             b.min, b.max, &b.has, &b.infinite) ==
              CLAY_ERROR_NOT_FOUND);
    }
}
