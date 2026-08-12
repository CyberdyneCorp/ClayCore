// The C ABI layer-node enumeration surface (#91, c-abi spec: a host can
// discover a layer's nodes). clay_layer_children answers for a GROUP, and a
// layer's root is not a group — it has no node id at all — so nothing listed
// the nodes a layer holds and a host that reloaded a document could not FIND
// the armature #77 made readable. The workaround was probing node ids from 1
// upward against clay_layer_node_prim and tolerating a run of misses, which
// loses every node past the longest gap it happened to tolerate; the gap case
// below is that failure made explicit.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

clay_node_id add_sphere_in_group(clay_document* doc, clay_layer_id layer, clay_node_id group) {
    clay_item_desc item;
    std::memset(&item, 0, sizeof item);
    item.struct_size = sizeof item;
    item.prim = CLAY_PRIM_SPHERE;
    item.params[0] = 0.2f;
    item.rotation[3] = 1.0f;
    item.scale = 1.0f;
    item.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item_in_group(doc, layer, group, -1, &item, &node) == CLAY_OK);
    return node;
}

// The branching rig of #77, so what is recovered here is the same tree that
// issue made readable — a chain could be guessed from geometry, a branch not.
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

std::vector<clay_node_id> enumerate(const clay_document* doc, clay_layer_id layer) {
    size_t count = 0;
    REQUIRE(clay_layer_node_count(doc, layer, &count) == CLAY_OK);
    std::vector<clay_node_id> ids;
    for (size_t i = 0; i < count; ++i) {
        clay_node_id node = 0;
        REQUIRE(clay_layer_node_at(doc, layer, i, &node) == CLAY_OK);
        ids.push_back(node);
    }
    return ids;
}

}  // namespace

TEST_CASE("a layer's nodes enumerate in evaluation order") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);

    size_t count = 99;
    REQUIRE(clay_layer_node_count(d.doc, layer, &count) == CLAY_OK);
    CHECK(count == 0);
    clay_node_id node = 0;
    CHECK(clay_layer_node_at(d.doc, layer, 0, &node) == CLAY_ERROR_NOT_FOUND);

    const clay_node_id a = add_sphere(d.doc, layer, 0.5f);
    const clay_node_id b = add_sphere(d.doc, layer, 0.4f);
    const clay_node_id c = add_sphere(d.doc, layer, 0.3f);
    CHECK(enumerate(d.doc, layer) == std::vector<clay_node_id>{a, b, c});

    // One past the end, exactly at the count boundary: how a host walks to the
    // end without a sentinel.
    CHECK(clay_layer_node_at(d.doc, layer, 3, &node) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("enumeration is top level only, and clay_layer_children descends") {
    // The documented division of labour, pinned: a group appears once at the
    // top level and its items do NOT, so the whole tree is walked by pairing
    // this call with clay_layer_children.
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id loose = add_sphere(d.doc, layer, 0.5f);
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(d.doc, layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f,
                                 &group) == CLAY_OK);
    const clay_node_id inner_a = add_sphere_in_group(d.doc, layer, group);
    const clay_node_id inner_b = add_sphere_in_group(d.doc, layer, group);

    CHECK(enumerate(d.doc, layer) == std::vector<clay_node_id>{loose, group});

    // The pairing itself: node_prim says which of the two enumerated nodes is
    // a group, and children descends into it.
    int32_t prim = -1;
    CHECK(clay_layer_node_prim(d.doc, layer, loose, &prim) == CLAY_OK);
    CHECK(prim == CLAY_PRIM_SPHERE);
    CHECK(clay_layer_node_prim(d.doc, layer, group, &prim) == CLAY_ERROR_INVALID_ARGUMENT);

    size_t children = 0;
    REQUIRE(clay_layer_children(d.doc, layer, group, nullptr, &children) == CLAY_OK);
    REQUIRE(children == 2);
    std::vector<clay_node_id> kids(children);
    REQUIRE(clay_layer_children(d.doc, layer, group, kids.data(), &children) == CLAY_OK);
    CHECK(kids == std::vector<clay_node_id>{inner_a, inner_b});

    // And the gap this pair fills: the layer root is not a group and has no
    // node id, so clay_layer_children cannot be asked about it.
    size_t at_root = 0;
    CHECK(clay_layer_children(d.doc, layer, 0, nullptr, &at_root) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("removed nodes leave id gaps that enumeration steps over") {
    // The case the probing workaround misses: ids are not dense, and nothing
    // bounds how long a run of missing ids can be. A probe that tolerates two
    // misses stops before the surviving node here; enumeration does not.
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id kept = add_sphere(d.doc, layer, 0.5f);
    std::vector<clay_node_id> doomed;
    for (int i = 0; i < 6; ++i) doomed.push_back(add_sphere(d.doc, layer, 0.1f));
    const clay_node_id survivor = add_sphere(d.doc, layer, 0.25f);
    for (clay_node_id id : doomed) REQUIRE(clay_remove_node(d.doc, layer, id) == CLAY_OK);

    CHECK(enumerate(d.doc, layer) == std::vector<clay_node_id>{kept, survivor});
    CHECK(survivor - kept > 2);  // the gap is longer than a probe would tolerate

    // The removed ids answer nothing, which is exactly why probing them is a
    // guess rather than an enumeration.
    int32_t prim = -1;
    for (clay_node_id id : doomed)
        CHECK(clay_layer_node_prim(d.doc, layer, id, &prim) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("a reloaded document finds its armature purely by enumeration") {
    // The regression: on main this test cannot be written at all — there is no
    // call that lists a layer's nodes, so a reloaded host has to probe ids from
    // 1 upward and guess how many misses to tolerate. Here the rig is placed
    // after a run of REMOVED nodes, which is the arrangement that defeats the
    // guess, and it is found without one.
    const std::string path = temp_path("c_layer_nodes_roundtrip.clayspace");
    clay_layer_id layer = 0;
    {
        Doc d;
        REQUIRE(clay_add_sdf_layer(d.doc, "corpo", &layer) == CLAY_OK);
        add_sphere(d.doc, layer, 0.5f);
        clay_node_id group = 0;
        REQUIRE(clay_layer_add_group(d.doc, layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f,
                                     0.0f, &group) == CLAY_OK);
        add_sphere_in_group(d.doc, layer, group);
        std::vector<clay_node_id> doomed;
        for (int i = 0; i < 8; ++i) doomed.push_back(add_sphere(d.doc, layer, 0.1f));
        for (clay_node_id id : doomed) REQUIRE(clay_remove_node(d.doc, layer, id) == CLAY_OK);
        place_rig(d.doc, layer);
        // Hidden and ghosted, because reading is not editing and the host that
        // reopens a document does not get to choose what state it was left in.
        REQUIRE(clay_document_set_layer_visible(d.doc, layer, 0) == CLAY_OK);
        REQUIRE(clay_document_set_layer_protection(d.doc, layer, 1, 1) == CLAY_OK);
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    Doc d(back);
    std::filesystem::remove(path);

    // Discovery from nothing but the document: which layers, then which nodes,
    // then what each node is. No id probing, no gap constant anywhere.
    size_t layer_count = 0;
    REQUIRE(clay_document_layer_count(d.doc, &layer_count) == CLAY_OK);
    REQUIRE(layer_count == 1);
    clay_layer_id found_layer = 0;
    REQUIRE(clay_document_layer_at(d.doc, 0, &found_layer) == CLAY_OK);

    clay_node_id armature = 0;
    size_t node_count = 0;
    REQUIRE(clay_layer_node_count(d.doc, found_layer, &node_count) == CLAY_OK);
    CHECK(node_count == 3);  // the sphere, the group, the rig
    for (size_t i = 0; i < node_count; ++i) {
        clay_node_id node = 0;
        REQUIRE(clay_layer_node_at(d.doc, found_layer, i, &node) == CLAY_OK);
        int32_t prim = -1;
        // A group refuses here, which is how the walk tells the two apart.
        if (clay_layer_node_prim(d.doc, found_layer, node, &prim) != CLAY_OK) continue;
        if (prim == CLAY_PRIM_ARMATURE) armature = node;
    }
    REQUIRE(armature != 0);

    // And what was found is the rig that was saved, branch included.
    float xyzr[16] = {0};
    size_t points = 4;
    REQUIRE(clay_layer_stroke_points(d.doc, found_layer, armature, xyzr, &points, nullptr,
                                     nullptr, nullptr, nullptr, nullptr) == CLAY_OK);
    CHECK(points == 4);
    CHECK(std::memcmp(xyzr, kRig, sizeof kRig) == 0);
    uint32_t parents[4] = {9, 9, 9, 9};
    size_t parent_count = 4;
    REQUIRE(clay_layer_armature_parents(d.doc, found_layer, armature, parents, &parent_count) ==
            CLAY_OK);
    for (int i = 0; i < 4; ++i) CHECK(parents[i] == kParents[i]);
}

TEST_CASE("the node enumerators keep their refusals typed") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    add_sphere(d.doc, layer, 0.5f);

    // An id that is not a layer's — including a NODE id passed where a layer
    // id belongs, the confusion two uint32 typedefs invite.
    size_t count = 99;
    CHECK(clay_layer_node_count(d.doc, layer + 77, &count) == CLAY_ERROR_NOT_FOUND);
    clay_node_id node = 0;
    CHECK(clay_layer_node_at(d.doc, layer + 77, 0, &node) == CLAY_ERROR_NOT_FOUND);

    // Null out-params and null documents are arguments, not lookups.
    CHECK(clay_layer_node_count(d.doc, layer, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_at(d.doc, layer, 0, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_count(nullptr, layer, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_at(nullptr, layer, 0, &node) == CLAY_ERROR_INVALID_ARGUMENT);

    // A voxel layer carries no SDF content, so it holds no nodes — empty
    // rather than an error, the reading clay_layer_eval_points makes of it.
    clay_layer_id voxel = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(d.doc, "grelha", 0.05f, &voxel, &grid) == CLAY_OK);
    count = 99;
    REQUIRE(clay_layer_node_count(d.doc, voxel, &count) == CLAY_OK);
    CHECK(count == 0);
    CHECK(clay_layer_node_at(d.doc, voxel, 0, &node) == CLAY_ERROR_NOT_FOUND);
}
