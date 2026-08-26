// Reading a PLACED node back (#317, c-abi spec: what a placed node holds).
// Four calls write a node's state — transform, prim, colour, op/blend — and
// before this the only accessor was clay_layer_node_prim, which answers which
// primitive and nothing else. A host that let an artist place a primitive,
// move it with a manipulator and change its operation afterwards therefore
// kept its own table of those values in a second file beside the .clay, keyed
// by node id, and kept it correct across undo and redo by following the
// engine's history by depth. These cases are that table's replacement.

#include <doctest/doctest.h>

#include <cmath>
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

clay_node_id add_prim(clay_document* doc, clay_layer_id layer, int32_t prim, const float* params,
                      int count) {
    clay_item_desc item;
    std::memset(&item, 0, sizeof item);
    item.struct_size = sizeof item;
    item.prim = prim;
    for (int i = 0; i < count; ++i) item.params[i] = params[i];
    item.rotation[3] = 1.0f;
    item.scale = 1.0f;
    item.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &item, &node) == CLAY_OK);
    return node;
}

clay_node_id add_sphere(clay_document* doc, clay_layer_id layer, float radius) {
    return add_prim(doc, layer, CLAY_PRIM_SPHERE, &radius, 1);
}

// A placed rotation, read back and applied again. The node stores a
// quaternion, so what comes back is a representative of the rotation rather
// than the pair last written; re-applying it and reading again is what pins
// the readback as a fixed point.
struct Placement {
    float position[3] = {0, 0, 0};
    float axis[3] = {0, 0, 0};
    float angle = 0.0f;
    float scale = 0.0f;
};

Placement read_placement(const clay_document* doc, clay_layer_id layer, clay_node_id node) {
    Placement p;
    REQUIRE(clay_layer_node_transform(doc, layer, node, p.position, p.axis, &p.angle, &p.scale) ==
            CLAY_OK);
    return p;
}

}  // namespace

TEST_CASE("a placed node reads back the transform that was set on it") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer, 0.5f);

    // The repro from the issue: place at 0.9 with a scale of 1.25, then ask.
    const float position[3] = {0.9f, -0.25f, 0.1f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, position, axis, 0.0f, 1.25f) == CLAY_OK);

    const Placement p = read_placement(d.doc, layer, node);
    CHECK(p.position[0] == doctest::Approx(0.9f));
    CHECK(p.position[1] == doctest::Approx(-0.25f));
    CHECK(p.position[2] == doctest::Approx(0.1f));
    CHECK(p.scale == doctest::Approx(1.25f));
    CHECK(p.angle == doctest::Approx(0.0f));
    // An unrotated item still names an axis, because clay_layer_set_transform
    // refuses a zero one and the readback has to feed straight back in.
    CHECK(p.axis[0] * p.axis[0] + p.axis[1] * p.axis[1] + p.axis[2] * p.axis[2] ==
          doctest::Approx(1.0f));
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, p.position, p.axis, p.angle, p.scale) ==
            CLAY_OK);

    // Every out-pointer is optional, including all of them at once — which is
    // how a host asks whether an id is still a node of that layer.
    CHECK(clay_layer_node_transform(d.doc, layer, node, nullptr, nullptr, nullptr, nullptr) ==
          CLAY_OK);
    float only_scale = 0.0f;
    CHECK(clay_layer_node_transform(d.doc, layer, node, nullptr, nullptr, nullptr, &only_scale) ==
          CLAY_OK);
    CHECK(only_scale == doctest::Approx(1.25f));
}

TEST_CASE("a rotation round trips through the axis-angle readback") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer, 0.5f);

    const float position[3] = {0.2f, 0.0f, -0.4f};
    // A non-unit axis, because the setter normalizes one and the reader has to
    // answer in the normalized frame rather than echoing what was passed.
    const float axis[3] = {0.0f, 2.0f, 2.0f};
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, position, axis, 1.1f, 0.75f) == CLAY_OK);

    const Placement first = read_placement(d.doc, layer, node);
    CHECK(first.angle == doctest::Approx(1.1f));
    const float inv = 1.0f / std::sqrt(8.0f);
    CHECK(first.axis[0] == doctest::Approx(0.0f));
    CHECK(first.axis[1] == doctest::Approx(2.0f * inv));
    CHECK(first.axis[2] == doctest::Approx(2.0f * inv));

    // Applying what came back and reading again is the fixed point: the pair
    // is a representative of the rotation, so it has to be a stable one.
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, first.position, first.axis, first.angle,
                                     first.scale) == CLAY_OK);
    const Placement second = read_placement(d.doc, layer, node);
    CHECK(second.angle == doctest::Approx(first.angle));
    for (int i = 0; i < 3; ++i) CHECK(second.axis[i] == doctest::Approx(first.axis[i]));

    // A turn past pi is the same rotation named the other way: it comes back
    // inside [0, pi] about the flipped axis, and evaluates identically.
    const float z[3] = {0.0f, 0.0f, 1.0f};
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, position, z, 4.0f, 1.0f) == CLAY_OK);
    const Placement wide = read_placement(d.doc, layer, node);
    CHECK(wide.angle == doctest::Approx(2.0f * 3.14159265f - 4.0f).epsilon(1e-4));
    CHECK(wide.axis[2] == doctest::Approx(-1.0f));
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, wide.position, wide.axis, wide.angle,
                                     wide.scale) == CLAY_OK);
    const Placement again = read_placement(d.doc, layer, node);
    CHECK(again.angle == doctest::Approx(wide.angle));
    for (int i = 0; i < 3; ++i) CHECK(again.axis[i] == doctest::Approx(wide.axis[i]));
}

TEST_CASE("a primitive's parameters read back by the size-query pattern") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const float box[3] = {0.3f, 0.4f, 0.5f};
    const clay_node_id node = add_prim(d.doc, layer, CLAY_PRIM_BOX, box, 3);

    // Count first, with no buffer at all.
    size_t count = 99;
    REQUIRE(clay_layer_node_params(d.doc, layer, node, nullptr, &count) == CLAY_OK);
    CHECK(count == 3);

    // A buffer that is too small says how big it should have been and writes
    // nothing, as clay_layer_children and clay_layer_stroke_points do.
    float two[2] = {-1.0f, -1.0f};
    size_t small = 2;
    CHECK(clay_layer_node_params(d.doc, layer, node, two, &small) == CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(small == 3);
    CHECK(two[0] == -1.0f);

    std::vector<float> params(count);
    REQUIRE(clay_layer_node_params(d.doc, layer, node, params.data(), &count) == CLAY_OK);
    CHECK(count == 3);
    for (int i = 0; i < 3; ++i) CHECK(params[static_cast<size_t>(i)] == doctest::Approx(box[i]));

    // And what came back goes straight back in, which is the point of reading
    // it: clay_layer_set_prim takes exactly this block.
    REQUIRE(clay_layer_set_prim(d.doc, layer, node, CLAY_PRIM_BOX, params.data(), count) ==
            CLAY_OK);

    // A replaced primitive changes the arity as well as the values, so the
    // count is the CURRENT primitive's and not the one the node was born with.
    const float torus[2] = {0.4f, 0.1f};
    REQUIRE(clay_layer_set_prim(d.doc, layer, node, CLAY_PRIM_TORUS, torus, 2) == CLAY_OK);
    size_t after = 0;
    REQUIRE(clay_layer_node_params(d.doc, layer, node, nullptr, &after) == CLAY_OK);
    CHECK(after == 2);
    int32_t prim = -1;
    REQUIRE(clay_layer_node_prim(d.doc, layer, node, &prim) == CLAY_OK);
    CHECK(prim == CLAY_PRIM_TORUS);
}

TEST_CASE("a primitive whose payload is out of line counts no parameters") {
    // A stroke's points are read by clay_layer_stroke_points, not here, and
    // this call says so by counting 0 rather than by refusing: the walk that
    // asks every node for its parameters should not have to special-case the
    // kinds whose payload lives elsewhere.
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const float points[8] = {0.0f, 0.0f, 0.0f, 0.2f, 0.5f, 0.0f, 0.0f, 0.15f};
    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_stroke_points(item, points, 2) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(d.doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    size_t count = 99;
    REQUIRE(clay_layer_node_params(d.doc, layer, node, nullptr, &count) == CLAY_OK);
    CHECK(count == 0);
    // A zero count with a real buffer is not a buffer that is too small.
    float scratch[1] = {-1.0f};
    size_t capacity = 1;
    REQUIRE(clay_layer_node_params(d.doc, layer, node, scratch, &capacity) == CLAY_OK);
    CHECK(capacity == 0);
    CHECK(scratch[0] == -1.0f);
}

TEST_CASE("a placed node reads back its op, blend and rounding") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer, 0.5f);

    REQUIRE(clay_layer_set_op_blend(d.doc, layer, node, CLAY_OP_SUBTRACT, CLAY_BLEND_QUADRATIC,
                                    0.08f, 0.02f) == CLAY_OK);
    int32_t op = -1, blend = -1;
    float blend_k = -1.0f, rounding = -1.0f;
    REQUIRE(clay_layer_node_op_blend(d.doc, layer, node, &op, &blend, &blend_k, &rounding) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_SUBTRACT);
    CHECK(blend == CLAY_BLEND_QUADRATIC);
    CHECK(blend_k == doctest::Approx(0.08f));
    CHECK(rounding == doctest::Approx(0.02f));
    REQUIRE(clay_layer_set_op_blend(d.doc, layer, node, op, blend, blend_k, rounding) == CLAY_OK);

    // Each out-pointer is optional here too.
    CHECK(clay_layer_node_op_blend(d.doc, layer, node, nullptr, nullptr, nullptr, nullptr) ==
          CLAY_OK);
}

TEST_CASE("a group answers for its op and refuses what it does not hold") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(d.doc, layer, 0, -1, CLAY_OP_INTERSECT, CLAY_BLEND_QUADRATIC,
                                 0.05f, 0.0f, &group) == CLAY_OK);

    // A group carries an op and a blend, and clay_layer_set_op_blend writes
    // them, so this is the one of the three that answers for both kinds.
    int32_t op = -1, blend = -1;
    float blend_k = -1.0f, rounding = -1.0f;
    REQUIRE(clay_layer_node_op_blend(d.doc, layer, group, &op, &blend, &blend_k, &rounding) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_INTERSECT);
    CHECK(blend == CLAY_BLEND_QUADRATIC);
    CHECK(blend_k == doctest::Approx(0.05f));
    CHECK(rounding == doctest::Approx(0.0f));

    // It has no transform of its own — the refusal its setter already makes —
    // and no primitive, so no parameter block either.
    float position[3] = {-1, -1, -1};
    CHECK(clay_layer_node_transform(d.doc, layer, group, position, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(position[0] == -1.0f);
    size_t count = 99;
    CHECK(clay_layer_node_params(d.doc, layer, group, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // An INLINE group reads back the blend it was required to be created with,
    // which it consults for nothing.
    clay_node_id inlined = 0;
    REQUIRE(clay_layer_add_group(d.doc, layer, 0, -1, CLAY_OP_INLINE, CLAY_BLEND_HARD, 0.0f, 0.0f,
                                 &inlined) == CLAY_OK);
    REQUIRE(clay_layer_node_op_blend(d.doc, layer, inlined, &op, &blend, &blend_k, &rounding) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_INLINE);
    CHECK(blend == CLAY_BLEND_HARD);
    CHECK(blend_k == doctest::Approx(0.0f));
}

TEST_CASE("the readers keep their refusals typed") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer, 0.5f);

    float position[3] = {0, 0, 0};
    size_t count = 0;
    int32_t op = 0;

    // An id that is not a layer's — including a node id where a layer id
    // belongs, the confusion two uint32 typedefs invite.
    CHECK(clay_layer_node_transform(d.doc, layer + 77, node, position, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_node_params(d.doc, layer + 77, node, nullptr, &count) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_node_op_blend(d.doc, layer + 77, node, &op, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);

    // A node the layer does not hold, including the no-node sentinel and an
    // id left behind by a removal.
    CHECK(clay_layer_node_transform(d.doc, layer, 0, position, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_node_params(d.doc, layer, node + 99, nullptr, &count) == CLAY_ERROR_NOT_FOUND);
    const clay_node_id doomed = add_sphere(d.doc, layer, 0.1f);
    REQUIRE(clay_remove_node(d.doc, layer, doomed) == CLAY_OK);
    CHECK(clay_layer_node_op_blend(d.doc, layer, doomed, &op, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);

    // A null document is an argument, not a lookup, and so is the one
    // out-pointer that is not optional.
    CHECK(clay_layer_node_transform(nullptr, layer, node, position, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_params(nullptr, layer, node, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_op_blend(nullptr, layer, node, &op, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_params(d.doc, layer, node, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A voxel layer holds no SDF nodes, so a node id cannot name one in it.
    clay_layer_id voxel = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(d.doc, "grelha", 0.05f, &voxel, &grid) == CLAY_OK);
    CHECK(clay_layer_node_transform(d.doc, voxel, node, position, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("a reloaded document reports placement the influence bound cannot") {
    // The whole issue in one case. The host's side-car existed because
    // clay_layer_node_influence_bound is the only thing that ever answered
    // anything positional, and it is not a position: it is dilated by rounding
    // and blend support, and under a layer mirror it covers the reflection
    // too, so an item at x = 0.9 in a mirrored layer reports a bound centred
    // on the origin. Reading is not editing, so the document is saved hidden,
    // ghosted and locked — the state a host does not get to choose when it
    // reopens someone else's file.
    const std::string path = temp_path("c_node_readback_roundtrip.clayspace");
    {
        Doc d;
        clay_layer_id layer = 0;
        REQUIRE(clay_add_sdf_layer(d.doc, "corpo", &layer) == CLAY_OK);
        const float box[3] = {0.2f, 0.3f, 0.4f};
        const clay_node_id node = add_prim(d.doc, layer, CLAY_PRIM_BOX, box, 3);
        const float position[3] = {0.9f, 0.0f, 0.0f};
        const float axis[3] = {0.0f, 1.0f, 0.0f};
        REQUIRE(clay_layer_set_transform(d.doc, layer, node, position, axis, 0.5f, 1.25f) ==
                CLAY_OK);
        REQUIRE(clay_layer_set_op_blend(d.doc, layer, node, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                        0.06f, 0.03f) == CLAY_OK);
        REQUIRE(clay_set_layer_mirror(d.doc, layer, 1, 0, 0, 0.0f) == CLAY_OK);
        REQUIRE(clay_document_set_layer_visible(d.doc, layer, 0) == CLAY_OK);
        REQUIRE(clay_document_set_layer_protection(d.doc, layer, 1, 1) == CLAY_OK);
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    Doc d(back);
    std::filesystem::remove(path);

    // Discovery from nothing but the document, as #91 laid it out: which
    // layers, which nodes, what each node is — and now what each node holds.
    clay_layer_id layer = 0;
    REQUIRE(clay_document_layer_at(d.doc, 0, &layer) == CLAY_OK);
    size_t nodes = 0;
    REQUIRE(clay_layer_node_count(d.doc, layer, &nodes) == CLAY_OK);
    REQUIRE(nodes == 1);
    clay_node_id node = 0;
    REQUIRE(clay_layer_node_at(d.doc, layer, 0, &node) == CLAY_OK);
    int32_t prim = -1;
    REQUIRE(clay_layer_node_prim(d.doc, layer, node, &prim) == CLAY_OK);
    CHECK(prim == CLAY_PRIM_BOX);

    const Placement p = read_placement(d.doc, layer, node);
    CHECK(p.position[0] == doctest::Approx(0.9f));
    CHECK(p.scale == doctest::Approx(1.25f));
    CHECK(p.angle == doctest::Approx(0.5f));
    CHECK(p.axis[1] == doctest::Approx(1.0f));

    size_t count = 0;
    REQUIRE(clay_layer_node_params(d.doc, layer, node, nullptr, &count) == CLAY_OK);
    REQUIRE(count == 3);
    std::vector<float> params(count);
    REQUIRE(clay_layer_node_params(d.doc, layer, node, params.data(), &count) == CLAY_OK);
    CHECK(params[0] == doctest::Approx(0.2f));
    CHECK(params[1] == doctest::Approx(0.3f));
    CHECK(params[2] == doctest::Approx(0.4f));

    int32_t op = -1, blend = -1;
    float blend_k = -1.0f, rounding = -1.0f;
    REQUIRE(clay_layer_node_op_blend(d.doc, layer, node, &op, &blend, &blend_k, &rounding) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_ADD);
    CHECK(blend == CLAY_BLEND_QUADRATIC);
    CHECK(blend_k == doctest::Approx(0.06f));
    CHECK(rounding == doctest::Approx(0.03f));

    // And the reading the host had to make do with. The bound is centred on
    // the origin because the mirror puts a copy at -0.9, and it reaches past
    // the box on every side because rounding and blend support dilate it — so
    // it names neither the position nor the size, which is why the second file
    // existed.
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    int32_t has_bounds = 0, infinite = 0;
    REQUIRE(clay_layer_node_influence_bound(d.doc, layer, node, lo, hi, &has_bounds, &infinite) ==
            CLAY_OK);
    REQUIRE(has_bounds == 1);
    REQUIRE(infinite == 0);
    const float centre = 0.5f * (lo[0] + hi[0]);
    CHECK(std::fabs(centre) < 0.1f);             // not 0.9
    CHECK(hi[1] - lo[1] > 2.0f * 0.3f * 1.25f);  // wider than the box it wraps
}
