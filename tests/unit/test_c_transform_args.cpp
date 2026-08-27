// The arguments every C ABI transform requires (#327). A host that only wants
// to MOVE a node reads the rotation_axis argument as optional and passes NULL
// for the rotation it does not have. It is not optional: these calls take the
// WHOLE transform, so a NULL that meant "no rotation" would also have to
// decide what happened to the position beside it, and the answer a partial
// update cannot give is the one that looks like success and moves nothing.
//
// The refusal is what a host has to check, so these cases pin it directly:
// the typed result, that the document is left exactly as it was, and — the
// part the issue was actually about — that the message names WHICH argument
// was missing, since "null transform" tells a caller only that one of two was.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_node_id add_sphere(clay_document* doc, clay_layer_id layer, float radius) {
    clay_item_desc item;
    std::memset(&item, 0, sizeof item);
    item.struct_size = sizeof item;
    item.prim = CLAY_PRIM_SPHERE;
    item.params[0] = radius;
    item.rotation[3] = 1.0f;
    item.scale = 1.0f;
    item.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &item, &node) == CLAY_OK);
    return node;
}

// Where the node stands, as its own reader answers. The comparison the issue
// made through clay_eval_points, without the sampling: what a refused edit
// must not have changed.
struct Placement {
    float position[3] = {0, 0, 0};
    float axis[3] = {0, 0, 0};
    float angle = 0.0f;
    float scale = 0.0f;
};

Placement placement_of(clay_document* doc, clay_layer_id layer, clay_node_id node) {
    Placement p;
    REQUIRE(clay_layer_node_transform(doc, layer, node, p.position, p.axis, &p.angle, &p.scale) ==
            CLAY_OK);
    return p;
}

bool same(const Placement& a, const Placement& b) {
    for (int i = 0; i < 3; ++i)
        if (a.position[i] != b.position[i] || a.axis[i] != b.axis[i]) return false;
    return a.angle == b.angle && a.scale == b.scale;
}

const float kOrigin[3] = {0, 0, 0};
const float kUpAxis[3] = {0, 1, 0};
const float kZeroAxis[3] = {0, 0, 0};

}  // namespace

TEST_CASE("a NULL rotation axis is refused, and the node keeps the placement it had") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "L", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer, 0.5f);

    // Placed first, so what the refused call must leave alone is a transform
    // somebody chose rather than the default one.
    const float placed[3] = {0.25f, 0.0f, -0.125f};
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, placed, kUpAxis, 0.5f, 1.5f) == CLAY_OK);
    const Placement before = placement_of(d.doc, layer, node);

    const float moved[3] = {-0.4f, 0.0f, 0.0f};
    CHECK(clay_layer_set_transform(d.doc, layer, node, moved, nullptr, 0.0f, 1.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // The position was carried by the same refused call, so this is the whole
    // of what the issue asked to be told about: nothing landed.
    CHECK(same(placement_of(d.doc, layer, node), before));

    // A zero axis is the same mistake spelled differently, and refuses too.
    CHECK(clay_layer_set_transform(d.doc, layer, node, moved, kZeroAxis, 0.0f, 1.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(same(placement_of(d.doc, layer, node), before));

    // And the way the signature already allows a host to say "no rotation"
    // does move it — the alternative the refusal points at.
    CHECK(clay_layer_set_transform(d.doc, layer, node, moved, kUpAxis, 0.0f, 1.0f) == CLAY_OK);
    const Placement after = placement_of(d.doc, layer, node);
    CHECK(after.position[0] == doctest::Approx(moved[0]));
    CHECK(after.angle == doctest::Approx(0.0f));
}

TEST_CASE("a missing transform argument is named, not reported as one of two") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "L", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer, 0.5f);

    REQUIRE(clay_layer_set_transform(d.doc, layer, node, kOrigin, nullptr, 0.0f, 1.0f) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    const std::string axis_error = clay_last_error();

    REQUIRE(clay_layer_set_transform(d.doc, layer, node, nullptr, kUpAxis, 0.0f, 1.0f) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    const std::string position_error = clay_last_error();

    // The point of the split: a caller cannot act on a message that describes
    // both mistakes equally well.
    CHECK(axis_error != position_error);
    CHECK(axis_error.find("axis") != std::string::npos);
    CHECK(position_error.find("position") != std::string::npos);
}

TEST_CASE("every transform entry point requires the same two arrays") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "L", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer, 0.5f);
    const float axes[3] = {1.0f, 1.0f, 1.0f};

    // One convention across the ABI, so a host cannot learn the rule from one
    // call and be caught by the next.
    CHECK(clay_layer_set_transform_nonuniform(d.doc, layer, node, kOrigin, nullptr, 0.0f, axes) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_transform_nonuniform(d.doc, layer, node, nullptr, kUpAxis, 0.0f, axes) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_transform(d.doc, layer, kOrigin, nullptr, 0.0f, 1.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_transform(d.doc, layer, nullptr, kUpAxis, 0.0f, 1.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_mesh_params params{};
    params.struct_size = sizeof params;
    params.resolution = 16;
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_document_mesh(d.doc, &params, &mesh) == CLAY_OK);
    clay_mesh* moved = reinterpret_cast<clay_mesh*>(0x1);
    CHECK(clay_mesh_transform(mesh, kOrigin, nullptr, 0.0f, 1.0f, &moved) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // A refused constructor clears its out-parameter rather than leaving the
    // caller's own value to be destroyed.
    CHECK(moved == nullptr);
    CHECK(clay_mesh_transform_nonuniform(mesh, kOrigin, nullptr, 0.0f, axes, &moved) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(moved == nullptr);
    clay_mesh_destroy(mesh);
}
