#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

// Instance layers across the C ABI (c-abi spec: "A layer can be instanced
// without copying its edit list", instance-a-layer).
//
// The engine could already share an edit list between layers — evaluation,
// bounds, culling and the memory report were all written for it — and nothing
// in the ABI could produce the state they described. What these cases hold is
// the things that had to be true before the constructor was worth shipping,
// and each is a property a separate revert can break:
//
//   * an edit through either layer is an edit to ONE edit list, so it is
//     visible through both and its dirty bounds cover both placements;
//   * a save and reload keeps the sharing, so the document's memory does not
//     scale with the instance count and the layers stay linked;
//   * consolidating one instance severs it and leaves the others alone;
//   * a gesture that states its own reach — the surface drag — states it for
//     every placement, not only the one whose layer was named;
//   * a reorder, which is a remove and an add, replays out of the journal
//     still sharing rather than as a deep copy.

namespace {

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

clay_layer_info info_of(const clay_document* doc, clay_layer_id layer) {
    clay_layer_info info{};
    info.struct_size = sizeof(info);
    REQUIRE(clay_document_layer_info(doc, layer, &info) == CLAY_OK);
    return info;
}

std::size_t node_count(const clay_document* doc, clay_layer_id layer) {
    std::size_t n = 0;
    REQUIRE(clay_layer_node_count(doc, layer, &n) == CLAY_OK);
    return n;
}

float distance_at(const clay_document* doc, float x, float y, float z) {
    const float p[3] = {x, y, z};
    float d = 0.0f;
    REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
    return d;
}

std::string name_of(const clay_document* doc, clay_layer_id layer) {
    std::size_t size = 0;
    REQUIRE(clay_layer_name(doc, layer, nullptr, &size) == CLAY_OK);
    std::vector<char> buffer(size);
    REQUIRE(clay_layer_name(doc, layer, buffer.data(), &size) == CLAY_OK);
    return std::string(buffer.data());
}

void place(clay_document* doc, clay_layer_id layer, float x) {
    const float position[3] = {x, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_document_set_layer_transform(doc, layer, position, axis, 0.0f, 1.0f) == CLAY_OK);
}

std::uint64_t document_edit_list(const clay_document* doc) {
    clay_memory_report m{};
    m.struct_size = sizeof(m);
    REQUIRE(clay_document_memory(doc, &m) == CLAY_OK);
    return m.edit_list;
}

std::uint64_t layer_edit_list(const clay_document* doc, clay_layer_id layer) {
    clay_memory_report m{};
    m.struct_size = sizeof(m);
    REQUIRE(clay_layer_memory(doc, layer, &m) == CLAY_OK);
    return m.edit_list;
}

// One brick of a host's cache, named by its key. A brick spans
// key * dim * spacing to that plus dim * spacing on each axis, and its lattice
// sample (i, j, k) sits at origin + (i, j, k) * spacing.
clay_brick_request brick_at(std::int32_t x, std::int32_t y, std::int32_t z) {
    clay_brick_request req{};
    req.key[0] = x;
    req.key[1] = y;
    req.key[2] = z;
    req.spacing = 0.125f;
    for (int axis = 0; axis < 3; ++axis) {
        req.dims[axis] = 8;
        req.origin[axis] = static_cast<float>(req.key[axis]) *
                           static_cast<float>(req.dims[axis]) * req.spacing;
    }
    req.band = 4.0f * req.spacing;
    return req;
}

std::size_t brick_samples(const clay_brick_request& req) {
    return static_cast<std::size_t>(req.dims[0]) * static_cast<std::size_t>(req.dims[1]) *
           static_cast<std::size_t>(req.dims[2]);
}

// The brick as a host would refill it — which is the call that consults the
// document's resume seeds, and so the call an under-invalidation shows up in.
std::vector<float> refill_brick(const clay_document* doc, const clay_brick_request& req) {
    std::vector<float> values(brick_samples(req));
    REQUIRE(clay_brick_cache_eval_requests(doc, nullptr, &req, 1, values.data(), values.size(),
                                           nullptr, 0) == CLAY_OK);
    return values;
}

// The same lattice evaluated point by point, which consults nothing and is
// therefore the ground truth to hold a refill against.
std::vector<float> sample_brick(const clay_document* doc, const clay_brick_request& req) {
    std::vector<float> points;
    points.reserve(brick_samples(req) * 3);
    for (std::int32_t k = 0; k < req.dims[2]; ++k)
        for (std::int32_t j = 0; j < req.dims[1]; ++j)
            for (std::int32_t i = 0; i < req.dims[0]; ++i) {
                points.push_back(req.origin[0] + static_cast<float>(i) * req.spacing);
                points.push_back(req.origin[1] + static_cast<float>(j) * req.spacing);
                points.push_back(req.origin[2] + static_cast<float>(k) * req.spacing);
            }
    std::vector<float> values(brick_samples(req));
    REQUIRE(clay_eval_points(doc, nullptr, points.data(), values.size(), values.data(),
                             nullptr) == CLAY_OK);
    return values;
}

float max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const float d = std::fabs(a[i] - b[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

// Save and reload through the buffer entry points, so a round trip costs no
// filesystem and the case reads as one document becoming another.
clay_document* round_trip(const clay_document* doc) {
    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc, &blob) == CLAY_OK);
    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob), &back) ==
            CLAY_OK);
    clay_blob_destroy(blob);
    REQUIRE(back != nullptr);
    return back;
}

}  // namespace

TEST_CASE("creating an instance is one undo step and copies nothing") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);

    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    CHECK(instance != source);
    CHECK(name_of(doc, instance) == "bolt");

    std::size_t layers = 0;
    REQUIRE(clay_document_layer_count(doc, &layers) == CLAY_OK);
    CHECK(layers == 2);
    // The instance holds the source's item without the source having gained
    // one: it is the SAME item, not a copy of it.
    CHECK(node_count(doc, instance) == 1);
    CHECK(node_count(doc, source) == 1);
    CHECK(info_of(doc, instance).share_count == 2);

    // ONE step. Through 0.57.0 there was no call to record; the point here is
    // that the add goes through the command vocabulary like every other layer
    // creation rather than mutating the document behind undo's back.
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_document_layer_count(doc, &layers) == CLAY_OK);
    CHECK(layers == 1);
    CHECK(node_count(doc, source) == 1);

    std::int32_t redone = 0;
    REQUIRE(clay_document_redo(doc, &redone) == CLAY_OK);
    CHECK(redone == 1);
    REQUIRE(clay_document_layer_count(doc, &layers) == CLAY_OK);
    CHECK(layers == 2);
    // Redone as an INSTANCE, not as a copy of one.
    CHECK(info_of(doc, instance).share_count == 2);

    clay_document_destroy(doc);
}

TEST_CASE("an edit through either layer is an edit to both") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    place(doc, instance, 5.0f);

    // Nothing at (5, 1.2, 0) yet: the instance carries one sphere at its own
    // origin, which the transform has put at x = 5.
    REQUIRE(distance_at(doc, 5.0f, 1.2f, 0.0f) > 0.0f);
    REQUIRE(distance_at(doc, 5.0f, 0.0f, 0.0f) < 0.0f);

    // ... added through the SOURCE, and the instance grows with it.
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_item_desc d{};
    d.struct_size = sizeof(d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = 0.5f;
    d.position[1] = 1.2f;
    d.rotation[3] = 1.0f;
    d.scale = 1.0f;
    d.op = CLAY_OP_ADD;
    REQUIRE(clay_add_item(doc, source, &d, nullptr) == CLAY_OK);
    CHECK(distance_at(doc, 5.0f, 1.2f, 0.0f) < 0.0f);
    CHECK(node_count(doc, instance) == node_count(doc, source));

    // ... and the other direction: added through the INSTANCE, and the source
    // grows with it. A shared edit list has no upstream end.
    d.position[1] = 0.0f;
    d.position[2] = 1.2f;
    REQUIRE(clay_add_item(doc, instance, &d, nullptr) == CLAY_OK);
    CHECK(distance_at(doc, 0.0f, 0.0f, 1.2f) < 0.0f);
    CHECK(node_count(doc, source) == node_count(doc, instance));

    clay_document_destroy(doc);
}

TEST_CASE("one edit list, two placements") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    place(doc, instance, 5.0f);

    CHECK(distance_at(doc, 0.0f, 0.0f, 0.0f) < 0.0f);
    CHECK(distance_at(doc, 5.0f, 0.0f, 0.0f) < 0.0f);
    CHECK(distance_at(doc, 2.5f, 0.0f, 0.0f) > 0.0f);  // and nothing between them

    clay_document_destroy(doc);
}

TEST_CASE("the instance's own properties start at the source's and diverge") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.3f, 0.8f);
    // Two properties set BEFORE the instance is made, so what the instance
    // carries can only have come from the source.
    REQUIRE(clay_set_layer_mirror(doc, source, 1, 0, 0, 0.0f) == CLAY_OK);
    REQUIRE(clay_document_set_layer_protection(doc, source, 1, 0) == CLAY_OK);

    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);

    // Copied at creation: the instance is ghosted like its source, and carries
    // the source's mirror.
    CHECK(info_of(doc, instance).ghost == 1);

    // Diverging afterwards leaves the source alone. Protection is released
    // first because a ghosted layer refuses every edit, which is the rule
    // being relied on rather than the one under test — and releasing it on
    // the instance leaves the source ghosted, which is the point.
    REQUIRE(clay_document_set_layer_protection(doc, instance, 0, 0) == CLAY_OK);
    place(doc, instance, 10.0f);
    // The mirror came across too: the instance's one sphere shows on both
    // sides of its own origin, ten units away.
    CHECK(distance_at(doc, 10.8f, 0.0f, 0.0f) < 0.0f);
    CHECK(distance_at(doc, 9.2f, 0.0f, 0.0f) < 0.0f);

    REQUIRE(clay_set_layer_mirror(doc, instance, 0, 0, 0, 0.0f) == CLAY_OK);
    CHECK(distance_at(doc, 9.2f, 0.0f, 0.0f) > 0.0f);  // and turning it off is local
    REQUIRE(clay_document_set_layer_visible(doc, instance, 0) == CLAY_OK);
    REQUIRE(clay_document_set_layer_name(doc, instance, "bolt 2") == CLAY_OK);

    clay_layer_info src_info = info_of(doc, source);
    CHECK(src_info.ghost == 1);
    CHECK(src_info.visible == 1);
    CHECK(name_of(doc, source) == "body");
    // The source keeps its mirror: the sphere it holds at +0.8 is still
    // reflected to -0.8.
    CHECK(distance_at(doc, -0.8f, 0.0f, 0.0f) < 0.0f);
    // ... and they still share the one edit list they started with.
    CHECK(info_of(doc, source).share_count == 2);

    clay_document_destroy(doc);
}

TEST_CASE("an edit's dirty bounds cover every placement of the shared content") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    const clay_node_id node = add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    place(doc, instance, 5.0f);

    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    std::int32_t has = 0, infinite = 0;
    REQUIRE(clay_layer_node_influence_bound(doc, source, node, lo, hi, &has, &infinite) ==
            CLAY_OK);
    REQUIRE(has == 1);
    REQUIRE(infinite == 0);
    // The box must reach the OTHER placement, or a host dirtying by it leaves
    // the instance holding stale bricks with nothing to say so.
    CHECK(lo[0] <= -0.5f);
    CHECK(hi[0] >= 5.0f);

    clay_document_destroy(doc);
}

TEST_CASE("a shared edit list is counted once, and stays that way across a save") {
    // Two documents that differ only in how many layers share one edit list.
    // Compared as a RATIO rather than as byte counts: sizeof(Node) and
    // bucket_count() differ between standard libraries.
    auto build = [](int instances) {
        clay_document* doc = clay_document_create();
        clay_layer_id source = 0;
        REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
        for (int i = 0; i < 200; ++i)
            add_sphere(doc, source, 0.2f, 0.05f * static_cast<float>(i));
        for (int i = 0; i < instances; ++i) {
            clay_layer_id made = 0;
            REQUIRE(clay_document_instance_layer(doc, source,
                                                 ("bolt " + std::to_string(i)).c_str(),
                                                 &made) == CLAY_OK);
            place(doc, made, 3.0f * static_cast<float>(i + 1));
        }
        return doc;
    };

    clay_document* alone = build(0);
    clay_document* ten = build(9);
    const std::uint64_t one_copy = document_edit_list(alone);
    // Non-degenerate: a ratio against a handful of bytes would mean nothing.
    REQUIRE(one_copy > 20000);
    CHECK(document_edit_list(ten) < one_copy * 3 / 2);

    // In FULL per layer, which is the other half of the contract: displaying
    // an instance costs an evaluation like any other layer.
    clay_layer_id first = 0, last = 0;
    REQUIRE(clay_document_layer_at(ten, 0, &first) == CLAY_OK);
    REQUIRE(clay_document_layer_at(ten, 9, &last) == CLAY_OK);
    CHECK(layer_edit_list(ten, last) == layer_edit_list(ten, first));
    REQUIRE(layer_edit_list(ten, last) > 20000);

    // AND AFTER A ROUND TRIP. Through 0.57.0 every layer's content went out
    // inline, so this reloaded ten times heavier and the layers were quietly
    // no longer linked.
    clay_document* reloaded = round_trip(ten);
    CHECK(document_edit_list(reloaded) < one_copy * 3 / 2);
    REQUIRE(clay_document_layer_at(reloaded, 0, &first) == CLAY_OK);
    REQUIRE(clay_document_layer_at(reloaded, 9, &last) == CLAY_OK);
    CHECK(layer_edit_list(reloaded, last) == layer_edit_list(reloaded, first));

    // The share itself survives, not only its cost: an edit through one of the
    // reloaded layers is visible through the others.
    CHECK(info_of(reloaded, last).content_source == first);
    add_sphere(reloaded, last, 0.2f, 40.0f);
    CHECK(node_count(reloaded, first) == node_count(reloaded, last));

    clay_document_destroy(alone);
    clay_document_destroy(ten);
    clay_document_destroy(reloaded);
}

TEST_CASE("layer info reports both ends of the link, and re-homes it") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id a = 0, b = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "a", &a) == CLAY_OK);
    REQUIRE(clay_document_instance_layer(doc, source, "b", &b) == CLAY_OK);

    CHECK(info_of(doc, source).content_source == 0);
    CHECK(info_of(doc, source).share_count == 3);
    CHECK(info_of(doc, a).content_source == source);
    CHECK(info_of(doc, b).content_source == source);

    // Unchanged by a save and reload, because the reported owner is derived by
    // the same rule the writer uses to decide whose record carries the bytes.
    clay_document* back = round_trip(doc);
    CHECK(info_of(back, source).content_source == 0);
    CHECK(info_of(back, a).content_source == source);
    CHECK(info_of(back, b).share_count == 3);
    clay_document_destroy(back);

    // Removing the layer that happened to be instanced re-homes the link
    // rather than dangling it: the first survivor becomes the owner.
    REQUIRE(clay_document_remove_layer(doc, source) == CLAY_OK);
    CHECK(info_of(doc, a).content_source == 0);
    CHECK(info_of(doc, a).share_count == 2);
    CHECK(info_of(doc, b).content_source == a);

    clay_document_destroy(doc);
}

TEST_CASE("a layer nothing instances shares with nobody") {
    clay_document* doc = clay_document_create();
    clay_layer_id plain = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &plain) == CLAY_OK);
    add_sphere(doc, plain, 0.5f, 0.0f);
    CHECK(info_of(doc, plain).content_source == 0);
    CHECK(info_of(doc, plain).share_count == 1);

    // A voxel layer holds no edit list at all, so it shares none.
    clay_layer_id grid = 0;
    REQUIRE(clay_document_add_voxel_layer(doc, "grid", 0.1f, &grid, nullptr) == CLAY_OK);
    CHECK(info_of(doc, grid).share_count == 0);
    CHECK(info_of(doc, grid).content_source == 0);

    clay_document_destroy(doc);
}

TEST_CASE("removing the source leaves the instance whole") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    place(doc, instance, 5.0f);

    REQUIRE(clay_document_remove_layer(doc, source) == CLAY_OK);
    // The content is held by every layer sharing it, so removing the layer
    // that was instanced removes a placement and nothing else.
    CHECK(node_count(doc, instance) == 1);
    CHECK(distance_at(doc, 5.0f, 0.0f, 0.0f) < 0.0f);
    CHECK(distance_at(doc, 0.0f, 0.0f, 0.0f) > 0.0f);

    clay_document* back = round_trip(doc);
    CHECK(node_count(back, instance) == 1);
    CHECK(distance_at(back, 5.0f, 0.0f, 0.0f) < 0.0f);
    clay_document_destroy(back);
    clay_document_destroy(doc);
}

TEST_CASE("instancing an instance shares the same edit list") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id first = 0, second = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "a", &first) == CLAY_OK);
    REQUIRE(clay_document_instance_layer(doc, first, "b", &second) == CLAY_OK);

    // Three layers over ONE edit list, not a chain of two relations.
    CHECK(info_of(doc, second).share_count == 3);
    CHECK(info_of(doc, second).content_source == source);
    add_sphere(doc, second, 0.5f, 2.0f);
    CHECK(node_count(doc, source) == 2);
    CHECK(node_count(doc, first) == 2);

    clay_document_destroy(doc);
}

TEST_CASE("instancing refuses what it cannot share") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id made = 12345;

    CHECK(clay_document_instance_layer(doc, 9999, "x", &made) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_instance_layer(nullptr, source, "x", &made) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_instance_layer(doc, source, nullptr, &made) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_instance_layer(doc, source, "", &made) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::strstr(clay_last_error(), "empty") != nullptr);

    // A voxel grid and a mesh are held beside the document by layer id rather
    // than behind the shared pointer an instance shares.
    clay_layer_id grid = 0;
    REQUIRE(clay_document_add_voxel_layer(doc, "grid", 0.1f, &grid, nullptr) == CLAY_OK);
    CHECK(clay_document_instance_layer(doc, grid, "x", &made) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::strstr(clay_last_error(), "voxel") != nullptr);

    const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::uint32_t indices[3] = {0, 1, 2};
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 3, indices, 3, &mesh) == CLAY_OK);
    clay_mesh_layer_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.name = "scan";
    clay_layer_id mesh_layer = 0;
    REQUIRE(clay_document_add_mesh_layer(doc, mesh, &desc, &mesh_layer, nullptr) == CLAY_OK);
    clay_mesh_destroy(mesh);
    CHECK(clay_document_instance_layer(doc, mesh_layer, "x", &made) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::strstr(clay_last_error(), "mesh") != nullptr);

    // Nothing was added by any of that.
    std::size_t layers = 0;
    REQUIRE(clay_document_layer_count(doc, &layers) == CLAY_OK);
    CHECK(layers == 3);
    CHECK(made == 12345);  // the out parameter is untouched on every refusal

    clay_document_destroy(doc);
}

TEST_CASE("consolidating an instance severs it and leaves the others parametric") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.6f, -0.3f);
    add_sphere(doc, source, 0.6f, 0.3f);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);

    clay_consolidation_params params{};
    params.struct_size = sizeof(params);
    params.cell_size = 0.1f;

    // Measuring changes nothing, sharing included.
    clay_consolidation_cost cost{};
    cost.struct_size = sizeof(cost);
    REQUIRE(clay_layer_consolidation_cost(doc, instance, &params, nullptr, nullptr, &cost) ==
            CLAY_OK);
    CHECK(info_of(doc, instance).share_count == 2);

    REQUIRE(clay_layer_consolidate(doc, instance, &params, nullptr, nullptr, nullptr) == CLAY_OK);
    // The bake landed on the instance ALONE.
    CHECK(node_count(doc, instance) == 1);
    CHECK(node_count(doc, source) == 2);
    CHECK(info_of(doc, instance).share_count == 1);
    CHECK(info_of(doc, source).share_count == 1);
    CHECK(info_of(doc, instance).content_source == 0);

    // And it stops following the source, which is what "finished" means.
    add_sphere(doc, source, 0.6f, 1.0f);
    CHECK(node_count(doc, source) == 3);
    CHECK(node_count(doc, instance) == 1);

    clay_document_destroy(doc);
}

TEST_CASE("undoing a consolidation restores the sharing") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.6f, -0.3f);
    add_sphere(doc, source, 0.6f, 0.3f);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);

    clay_consolidation_params params{};
    params.struct_size = sizeof(params);
    params.cell_size = 0.1f;
    REQUIRE(clay_layer_consolidate(doc, instance, &params, nullptr, nullptr, nullptr) == CLAY_OK);
    REQUIRE(info_of(doc, instance).share_count == 1);

    // ONE step: the sever and the bake go into the same group.
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(node_count(doc, instance) == 2);
    CHECK(info_of(doc, instance).share_count == 2);
    // Linked again, not merely equal: an edit through one reaches the other.
    add_sphere(doc, source, 0.6f, 1.0f);
    CHECK(node_count(doc, instance) == 3);

    clay_document_destroy(doc);
}

TEST_CASE("replaying an instance creation shares rather than copies") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
    for (int i = 0; i < 200; ++i) add_sphere(doc, source, 0.2f, 0.05f * static_cast<float>(i));

    // The snapshot, and the journal index it was taken at.
    clay_blob* snapshot = nullptr;
    REQUIRE(clay_document_save_memory(doc, &snapshot) == CLAY_OK);
    std::size_t at = 0;
    REQUIRE(clay_document_journal_range(doc, nullptr, &at) == CLAY_OK);

    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);

    clay_blob* journal = nullptr;
    std::size_t now_at = 0;
    REQUIRE(clay_document_journal_since(doc, at, &journal, &now_at) == CLAY_OK);

    clay_document* recovered = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(snapshot), clay_blob_size(snapshot),
                                      &recovered) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(recovered) == CLAY_OK);
    std::size_t applied = 0;
    std::int32_t barrier = 0;
    REQUIRE(clay_document_replay_journal(recovered, clay_blob_data(journal),
                                         clay_blob_size(journal), &applied, &barrier) == CLAY_OK);
    CHECK(barrier == 0);
    CHECK(applied >= 1);

    std::size_t layers = 0;
    REQUIRE(clay_document_layer_count(recovered, &layers) == CLAY_OK);
    CHECK(layers == 2);
    // The recovered instance SHARES: an AddLayerCmd carrying the content
    // inline would have replayed as a deep copy, and the recovery would have
    // quietly cost twice the edit list it was recovering.
    CHECK(info_of(recovered, instance).share_count == 2);
    CHECK(info_of(recovered, instance).content_source == source);
    CHECK(document_edit_list(recovered) < document_edit_list(doc) * 3 / 2);
    add_sphere(recovered, instance, 0.2f, 40.0f);
    CHECK(node_count(recovered, source) == 201);

    clay_blob_destroy(snapshot);
    clay_blob_destroy(journal);
    clay_document_destroy(recovered);
    clay_document_destroy(doc);
}

TEST_CASE("a drag through one placement dirties the others") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    add_sphere(doc, source, 0.5f, 0.0f);
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    place(doc, instance, 4.0f);

    // The refill agrees with the document before anything moves, so a
    // disagreement afterwards can only be the invalidation. Compared through
    // max_abs_diff rather than as two vectors: doctest decomposes a bare
    // `a == b` and stringifies each side, which is what made a shared_ptr
    // comparison fail MSVC's /W4 /WX job while libstdc++ compiled it.
    const clay_brick_request req = brick_at(4, 0, 0);
    const std::vector<float> before = refill_brick(doc, req);
    REQUIRE(max_abs_diff(before, sample_brick(doc, req)) == 0.0f);

    clay_move_params params{};
    params.struct_size = sizeof(params);
    params.radius = 1.0f;
    const float centre[3] = {0.0f, 0.5f, 0.0f};
    const float displacement[3] = {0.0f, 0.4f, 0.0f};
    std::size_t applied = 0;
    REQUIRE(clay_layer_move_surface(doc, source, centre, displacement, &params, &applied) ==
            CLAY_OK);
    REQUIRE(applied == 1);

    const std::vector<float> truth = sample_brick(doc, req);
    REQUIRE(max_abs_diff(truth, before) > 0.05f);  // the drag really reached here
    CHECK(max_abs_diff(refill_brick(doc, req), truth) < 1e-4f);

    clay_document_destroy(doc);
}

TEST_CASE("replaying a reorder of a shared layer keeps the sharing") {
    clay_document* doc = clay_document_create();
    clay_layer_id source = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &source) == CLAY_OK);
    for (int i = 0; i < 200; ++i) add_sphere(doc, source, 0.2f, 0.05f * static_cast<float>(i));
    clay_layer_id instance = 0;
    REQUIRE(clay_document_instance_layer(doc, source, "bolt", &instance) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);

    // The snapshot already holds both layers sharing, so the only thing the
    // journal has to carry is the reorder.
    clay_blob* snapshot = nullptr;
    REQUIRE(clay_document_save_memory(doc, &snapshot) == CLAY_OK);
    std::size_t at = 0;
    REQUIRE(clay_document_journal_range(doc, nullptr, &at) == CLAY_OK);

    // A reorder is a remove and an add. In memory the add carries the shared
    // pointer and nothing looks wrong; the journal serializes the command, and
    // an add that names no source writes the edit list inline.
    REQUIRE(clay_document_move_layer(doc, instance, 0) == CLAY_OK);
    CHECK(info_of(doc, instance).share_count == 2);

    clay_blob* journal = nullptr;
    std::size_t now_at = 0;
    REQUIRE(clay_document_journal_since(doc, at, &journal, &now_at) == CLAY_OK);

    clay_document* recovered = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(snapshot), clay_blob_size(snapshot),
                                      &recovered) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(recovered) == CLAY_OK);
    std::size_t applied = 0;
    std::int32_t barrier = 0;
    REQUIRE(clay_document_replay_journal(recovered, clay_blob_data(journal),
                                         clay_blob_size(journal), &applied, &barrier) == CLAY_OK);
    CHECK(barrier == 0);

    // The reorder happened, and the layers are still one edit list. Recovered
    // as copies, an artist who reordered a duplicated subtool would come back
    // to unlinked subtools and twice the edit list, with the shapes right and
    // nothing to see.
    clay_layer_id first = 0;
    REQUIRE(clay_document_layer_at(recovered, 0, &first) == CLAY_OK);
    CHECK(first == instance);
    CHECK(info_of(recovered, instance).share_count == 2);
    CHECK(info_of(recovered, source).share_count == 2);
    CHECK(document_edit_list(recovered) < document_edit_list(doc) * 3 / 2);
    add_sphere(recovered, instance, 0.2f, 40.0f);
    CHECK(node_count(recovered, source) == 201);

    clay_blob_destroy(snapshot);
    clay_blob_destroy(journal);
    clay_document_destroy(recovered);
    clay_document_destroy(doc);
}
