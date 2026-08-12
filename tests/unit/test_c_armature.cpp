// The C ABI armature readback surface (#77, c-abi spec: armatures across the
// ABI). A placed armature was write-only: clay_layer_stroke_points refused the
// primitive and nothing read the parents back, so a host that reloaded a
// document held the skinned surface and an unposable rig —
// clay_layer_armature_edit takes indices into a tree the host could not see,
// and branching topology is not recoverable from geometry. Pinned across the
// path that makes it matter: a save and reload of a rig that BRANCHES, since
// a chain could at least be guessed and a branch cannot.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    explicit Doc(clay_document* d) : doc(d) {}
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// The issue's exact rig: node 3 hangs off node 1, NOT off node 2, so the
// nearest-preceding-sphere guess a host could fall back on reads it wrong.
const float kRig[16] = {
    0.0f, 0.0f, 0.0f, 0.30f,  // 0, the root
    0.5f, 0.0f, 0.0f, 0.20f,  // 1, off the root
    1.0f, 0.0f, 0.0f, 0.15f,  // 2, off 1
    0.5f, 0.6f, 0.0f, 0.15f,  // 3, off 1 as well
};
const uint32_t kParents[4] = {0, 0, 1, 1};

clay_node_id place_rig(clay_document* doc, clay_layer_id layer) {
    clay_item* item = clay_item_create(CLAY_PRIM_ARMATURE, nullptr, 0);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_stroke_points(item, kRig, 4) == CLAY_OK);
    REQUIRE(clay_item_set_armature_parents(item, kParents, 4) == CLAY_OK);
    REQUIRE(clay_item_set_op(item, CLAY_OP_ADD) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);
    return node;
}

void read_rig(const clay_document* doc, clay_layer_id layer, clay_node_id node,
              float out_xyzr[16], uint32_t out_parents[4]) {
    size_t count = 4;
    REQUIRE(clay_layer_stroke_points(doc, layer, node, out_xyzr, &count, nullptr, nullptr,
                                     nullptr, nullptr, nullptr) == CLAY_OK);
    REQUIRE(count == 4);
    count = 4;
    REQUIRE(clay_layer_armature_parents(doc, layer, node, out_parents, &count) == CLAY_OK);
    REQUIRE(count == 4);
}

}  // namespace

TEST_CASE("c armature: a reloaded branching rig reads back and re-poses") {
    const std::string path = temp_path("c_armature_readback.clayspace");
    clay_layer_id layer = 0;
    clay_node_id node = 0;
    {
        Doc d;
        REQUIRE(clay_add_sdf_layer(d.doc, "rig", &layer) == CLAY_OK);
        node = place_rig(d.doc, layer);
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    Doc d(back);
    std::filesystem::remove(path);

    // The host can FIND the armature: the node answers what it is, instead of
    // being probed against readers until one stops refusing.
    int32_t prim = -1;
    REQUIRE(clay_layer_node_prim(d.doc, layer, node, &prim) == CLAY_OK);
    CHECK(prim == CLAY_PRIM_ARMATURE);

    // Size query first, the pattern clay_layer_children uses. out_closed is
    // answered on the size query too, and an armature has no curve settings.
    size_t count = 0;
    int32_t closed = 7;
    REQUIRE(clay_layer_stroke_points(d.doc, layer, node, nullptr, &count, nullptr, nullptr,
                                     nullptr, &closed, nullptr) == CLAY_OK);
    CHECK(count == 4);
    CHECK(closed == 0);
    count = 0;
    REQUIRE(clay_layer_armature_parents(d.doc, layer, node, nullptr, &count) == CLAY_OK);
    CHECK(count == 4);

    // Both halves come back exactly as authored — including the branch: node
    // 3's parent is 1, not the 2 a nearest-preceding-sphere guess would give.
    float xyzr[16] = {0};
    uint32_t parents[4] = {9, 9, 9, 9};
    read_rig(d.doc, layer, node, xyzr, parents);
    CHECK(std::memcmp(xyzr, kRig, sizeof kRig) == 0);
    for (int i = 0; i < 4; ++i) CHECK(parents[i] == kParents[i]);

    // The indices are the ones clay_layer_armature_edit takes: moving node 1
    // through the read-back tree carries ITS subtree — 2 and 3 both hang off
    // it — and leaves the root alone.
    const float delta[3] = {0.0f, 0.0f, 0.5f};
    REQUIRE(clay_layer_armature_edit(d.doc, layer, node, CLAY_ARMATURE_MOVE, 1, delta, 0.0f,
                                     0) == CLAY_OK);
    read_rig(d.doc, layer, node, xyzr, parents);
    CHECK(xyzr[0 * 4 + 2] == doctest::Approx(0.0f));  // root stayed
    CHECK(xyzr[1 * 4 + 2] == doctest::Approx(0.5f));
    CHECK(xyzr[2 * 4 + 2] == doctest::Approx(0.5f));
    CHECK(xyzr[3 * 4 + 2] == doctest::Approx(0.5f));

    // And the branch is real: node 2 is a LEAF of the read-back tree, so
    // moving it moves it alone. Under the mis-guessed chain 0,0,1,2 it would
    // have dragged node 3 with it.
    const float leaf_delta[3] = {0.0f, 0.0f, 0.25f};
    REQUIRE(clay_layer_armature_edit(d.doc, layer, node, CLAY_ARMATURE_MOVE, 2, leaf_delta,
                                     0.0f, 0) == CLAY_OK);
    read_rig(d.doc, layer, node, xyzr, parents);
    CHECK(xyzr[2 * 4 + 2] == doctest::Approx(0.75f));
    CHECK(xyzr[3 * 4 + 2] == doctest::Approx(0.5f));  // the sibling did not move
    for (int i = 0; i < 4; ++i) CHECK(parents[i] == kParents[i]);  // topology untouched
}

TEST_CASE("c armature: the parents reader keeps the refusals typed") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "l", &layer) == CLAY_OK);
    clay_node_id rig = place_rig(d.doc, layer);

    // A stroke has no parents: the same typed refusal the tree edits give,
    // not an empty answer that would read as a rig of roots.
    clay_item* stroke = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(stroke != nullptr);
    REQUIRE(clay_item_set_stroke_points(stroke, kRig, 4) == CLAY_OK);
    clay_node_id stroke_node = 0;
    REQUIRE(clay_layer_add_item(d.doc, layer, stroke, &stroke_node) == CLAY_OK);
    clay_item_destroy(stroke);
    size_t count = 0;
    CHECK(clay_layer_armature_parents(d.doc, layer, stroke_node, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A group is not an armature either, and a missing node is NOT_FOUND.
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(d.doc, layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f,
                                 0.0f, &group) == CLAY_OK);
    CHECK(clay_layer_armature_parents(d.doc, layer, group, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_armature_parents(d.doc, layer, 999999, nullptr, &count) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_armature_parents(d.doc, layer, rig, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A short buffer reports the needed count and writes nothing.
    uint32_t two[2] = {77, 77};
    count = 2;
    CHECK(clay_layer_armature_parents(d.doc, layer, rig, two, &count) ==
          CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(count == 4);
    CHECK(two[0] == 77);
    CHECK(two[1] == 77);

    // The point readback refuses prims that carry no point list, as before.
    clay_item_desc sphere;
    std::memset(&sphere, 0, sizeof sphere);
    sphere.struct_size = sizeof sphere;
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 0.5f;
    sphere.rotation[3] = 1.0f;
    sphere.scale = 1.0f;
    sphere.op = CLAY_OP_ADD;
    clay_node_id ball = 0;
    REQUIRE(clay_add_item(d.doc, layer, &sphere, &ball) == CLAY_OK);
    count = 0;
    CHECK(clay_layer_stroke_points(d.doc, layer, ball, nullptr, &count, nullptr, nullptr,
                                   nullptr, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    // Reading did not open a back door on the write side: replacing a placed
    // armature's points alone would desynchronise them from its parents, so
    // the placed-node setter still refuses and the tree edits own that half.
    CHECK(clay_layer_set_stroke_points(d.doc, layer, rig, kRig, 4, nullptr, nullptr, nullptr,
                                       0, 0.01f) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("c armature: a tree authored shorter than its points reads as evaluated") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "l", &layer) == CLAY_OK);

    // Four nodes, two parents: tape_build reads the missing tail as roots, so
    // the readback reports the same — the tree the document EVALUATES, with
    // the count always the node count the xyzr half reports.
    clay_item* item = clay_item_create(CLAY_PRIM_ARMATURE, nullptr, 0);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_stroke_points(item, kRig, 4) == CLAY_OK);
    const uint32_t two[2] = {0, 0};
    REQUIRE(clay_item_set_armature_parents(item, two, 2) == CLAY_OK);
    REQUIRE(clay_item_set_op(item, CLAY_OP_ADD) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(d.doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    size_t count = 0;
    REQUIRE(clay_layer_armature_parents(d.doc, layer, node, nullptr, &count) == CLAY_OK);
    REQUIRE(count == 4);
    uint32_t parents[4] = {9, 9, 9, 9};
    REQUIRE(clay_layer_armature_parents(d.doc, layer, node, parents, &count) == CLAY_OK);
    CHECK(parents[0] == 0);
    CHECK(parents[1] == 0);
    CHECK(parents[2] == 2);  // absent, so a root — the reading compilation makes
    CHECK(parents[3] == 3);
}

TEST_CASE("c armature: a node answers what primitive it carries") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "l", &layer) == CLAY_OK);
    clay_node_id rig = place_rig(d.doc, layer);

    int32_t prim = -1;
    REQUIRE(clay_layer_node_prim(d.doc, layer, rig, &prim) == CLAY_OK);
    CHECK(prim == CLAY_PRIM_ARMATURE);

    clay_item_desc sphere;
    std::memset(&sphere, 0, sizeof sphere);
    sphere.struct_size = sizeof sphere;
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 0.5f;
    sphere.rotation[3] = 1.0f;
    sphere.scale = 1.0f;
    sphere.op = CLAY_OP_ADD;
    clay_node_id ball = 0;
    REQUIRE(clay_add_item(d.doc, layer, &sphere, &ball) == CLAY_OK);
    REQUIRE(clay_layer_node_prim(d.doc, layer, ball, &prim) == CLAY_OK);
    CHECK(prim == CLAY_PRIM_SPHERE);

    // A group carries no primitive — the dual of clay_layer_children refusing
    // an item, so every node answers exactly one of the two questions.
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(d.doc, layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f,
                                 0.0f, &group) == CLAY_OK);
    CHECK(clay_layer_node_prim(d.doc, layer, group, &prim) == CLAY_ERROR_INVALID_ARGUMENT);
    size_t count = 0;
    CHECK(clay_layer_children(d.doc, layer, group, nullptr, &count) == CLAY_OK);
    CHECK(clay_layer_children(d.doc, layer, rig, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    CHECK(clay_layer_node_prim(d.doc, layer, 999999, &prim) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_node_prim(d.doc, layer, rig, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Signs (#99, add-armature-node-signs): a negative node carves instead of
// skinning, and the sign travels with the tree — setter, placed edit, readback
// and file — so a reopened rig can be un-negatived.

namespace {

clay_node_id place_signed_rig(clay_document* doc, clay_layer_id layer) {
    clay_item* item = clay_item_create(CLAY_PRIM_ARMATURE, nullptr, 0);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_stroke_points(item, kRig, 4) == CLAY_OK);
    REQUIRE(clay_item_set_armature_parents(item, kParents, 4) == CLAY_OK);
    const int8_t signs[4] = {1, -1, 1, 1};
    REQUIRE(clay_item_set_armature_signs(item, signs, 4) == CLAY_OK);
    REQUIRE(clay_item_set_op(item, CLAY_OP_ADD) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);
    return node;
}

}  // namespace

TEST_CASE("c armature: a negative node survives the round trip and flips back") {
    const std::string path = temp_path("c_armature_signs.clayspace");
    clay_layer_id layer = 0;
    clay_node_id node = 0;
    {
        Doc d;
        REQUIRE(clay_add_sdf_layer(d.doc, "rig", &layer) == CLAY_OK);
        node = place_signed_rig(d.doc, layer);
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    Doc d(back);
    std::filesystem::remove(path);

    // The issue's second defect, closed: the reopened rig knows which node is
    // negative. Size query first, by the pattern the parents readback set.
    size_t count = 0;
    REQUIRE(clay_layer_armature_signs(d.doc, layer, node, nullptr, &count) == CLAY_OK);
    REQUIRE(count == 4);
    int8_t signs[4] = {0, 0, 0, 0};
    REQUIRE(clay_layer_armature_signs(d.doc, layer, node, signs, &count) == CLAY_OK);
    CHECK(signs[0] == 1);
    CHECK(signs[1] == -1);
    CHECK(signs[2] == 1);
    CHECK(signs[3] == 1);

    // And it can be UN-negatived — what the workaround could not do at all.
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    REQUIRE(clay_layer_armature_edit(d.doc, layer, node, CLAY_ARMATURE_SET_SIGN, 1, nullptr,
                                     1.0f, 0) == CLAY_OK);
    REQUIRE(clay_layer_armature_signs(d.doc, layer, node, signs, &count) == CLAY_OK);
    for (int i = 0; i < 4; ++i) CHECK(signs[i] == 1);

    // One undo step, and the sign is back.
    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    REQUIRE(undone == 1);
    REQUIRE(clay_layer_armature_signs(d.doc, layer, node, signs, &count) == CLAY_OK);
    CHECK(signs[1] == -1);
}

TEST_CASE("c armature: a negative node is not required to be a leaf") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "l", &layer) == CLAY_OK);
    clay_node_id rig = place_rig(d.doc, layer);

    // Node 1 carries 2 and 3 — the issue's third defect was having to refuse
    // exactly this. The edit succeeds, and moving the negative node still
    // carries its subtree, signs undisturbed.
    REQUIRE(clay_layer_armature_edit(d.doc, layer, rig, CLAY_ARMATURE_SET_SIGN, 1, nullptr,
                                     -1.0f, 0) == CLAY_OK);
    const float delta[3] = {0.0f, 0.0f, 0.5f};
    REQUIRE(clay_layer_armature_edit(d.doc, layer, rig, CLAY_ARMATURE_MOVE, 1, delta, 0.0f,
                                     0) == CLAY_OK);
    float xyzr[16] = {0};
    uint32_t parents[4] = {0};
    read_rig(d.doc, layer, rig, xyzr, parents);
    CHECK(xyzr[1 * 4 + 2] == doctest::Approx(0.5f));
    CHECK(xyzr[3 * 4 + 2] == doctest::Approx(0.5f));  // the subtree came along
    size_t count = 4;
    int8_t signs[4] = {0};
    REQUIRE(clay_layer_armature_signs(d.doc, layer, rig, signs, &count) == CLAY_OK);
    CHECK(signs[1] == -1);

    // Deleting the negative subtree renumbers the survivors' signs with them.
    REQUIRE(clay_layer_armature_edit(d.doc, layer, rig, CLAY_ARMATURE_DELETE, 1, nullptr, 0.0f,
                                     0) == CLAY_OK);
    count = 4;
    REQUIRE(clay_layer_armature_signs(d.doc, layer, rig, signs, &count) == CLAY_OK);
    CHECK(count == 1);
    CHECK(signs[0] == 1);
}

TEST_CASE("c armature: the signs surface keeps the refusals typed") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "l", &layer) == CLAY_OK);
    clay_node_id rig = place_rig(d.doc, layer);

    // The setter: only +1 and -1 are signs. A zero or a magnitude is refused,
    // not coerced — the negative-radius convention deliberately not taken.
    clay_item* item = clay_item_create(CLAY_PRIM_ARMATURE, nullptr, 0);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_stroke_points(item, kRig, 4) == CLAY_OK);
    const int8_t zero[4] = {1, 0, 1, 1};
    CHECK(clay_item_set_armature_signs(item, zero, 4) == CLAY_ERROR_INVALID_ARGUMENT);
    const int8_t two[4] = {1, 2, 1, 1};
    CHECK(clay_item_set_armature_signs(item, two, 4) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_armature_signs(item, nullptr, 4) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(item);

    // A stroke has no signs, and the sign is not a prim property of anything
    // else either — the same typed refusal the parents reader gives.
    clay_item* stroke = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(stroke != nullptr);
    const int8_t ok_signs[4] = {1, -1, 1, 1};
    CHECK(clay_item_set_armature_signs(stroke, ok_signs, 4) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_stroke_points(stroke, kRig, 4) == CLAY_OK);
    clay_node_id stroke_node = 0;
    REQUIRE(clay_layer_add_item(d.doc, layer, stroke, &stroke_node) == CLAY_OK);
    clay_item_destroy(stroke);
    size_t count = 0;
    CHECK(clay_layer_armature_signs(d.doc, layer, stroke_node, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_armature_signs(d.doc, layer, 999999, nullptr, &count) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_armature_signs(d.doc, layer, rig, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A short buffer reports the needed count and writes nothing.
    int8_t small[2] = {77, 77};
    count = 2;
    CHECK(clay_layer_armature_signs(d.doc, layer, rig, small, &count) ==
          CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(count == 4);
    CHECK(small[0] == 77);
    CHECK(small[1] == 77);

    // The placed edit: a sign that is not exactly +/-1 is refused, and so is a
    // node that is not there.
    CHECK(clay_layer_armature_edit(d.doc, layer, rig, CLAY_ARMATURE_SET_SIGN, 1, nullptr, 0.5f,
                                   0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_armature_edit(d.doc, layer, rig, CLAY_ARMATURE_SET_SIGN, 99, nullptr,
                                   -1.0f, 0) == CLAY_ERROR_INVALID_ARGUMENT);

    // Protection refuses the EDIT but not the read.
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 1) == CLAY_OK);
    CHECK(clay_layer_armature_edit(d.doc, layer, rig, CLAY_ARMATURE_SET_SIGN, 1, nullptr, -1.0f,
                                   0) == CLAY_ERROR_INVALID_ARGUMENT);
    count = 4;
    int8_t signs[4] = {0};
    CHECK(clay_layer_armature_signs(d.doc, layer, rig, signs, &count) == CLAY_OK);
    CHECK(signs[1] == 1);  // the refused edit changed nothing
}

TEST_CASE("c armature: signs authored shorter than the nodes read as evaluated") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "l", &layer) == CLAY_OK);

    // No signs set at all — every armature authored before signs existed.
    clay_node_id rig = place_rig(d.doc, layer);
    size_t count = 0;
    REQUIRE(clay_layer_armature_signs(d.doc, layer, rig, nullptr, &count) == CLAY_OK);
    REQUIRE(count == 4);
    int8_t signs[4] = {0, 0, 0, 0};
    REQUIRE(clay_layer_armature_signs(d.doc, layer, rig, signs, &count) == CLAY_OK);
    for (int i = 0; i < 4; ++i) CHECK(signs[i] == 1);  // positive-padded
}
