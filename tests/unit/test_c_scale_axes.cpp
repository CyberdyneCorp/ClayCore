// The per-axis scale across the C ABI (#320, c-abi spec).
//
// Every transform in the interface took one float, and the shapes a boolean
// workflow cuts with are mostly not uniform. These cases pin the surface: what
// the two setters mean against each other, what the uniform reader does when it
// cannot express what is there, and the mesh arm, where there is no field and
// the whole question is what happens to the normals.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_node_id add_sphere(clay_document* doc, clay_layer_id layer) {
    clay_item_desc item;
    std::memset(&item, 0, sizeof item);
    item.struct_size = sizeof item;
    item.prim = CLAY_PRIM_SPHERE;
    item.params[0] = 1.0f;
    item.rotation[3] = 1.0f;
    item.scale = 1.0f;
    item.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &item, &node) == CLAY_OK);
    return node;
}

float eval_one(clay_document* doc, float x, float y, float z) {
    const float p[3] = {x, y, z};
    float d = 0.0f;
    REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
    return d;
}

const float kIdentityAxis[3] = {0.0f, 1.0f, 0.0f};
const float kOrigin[3] = {0.0f, 0.0f, 0.0f};

}  // namespace

TEST_CASE("a placed node takes a per-axis scale and evaluates squashed") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer);

    // The issue's own shape: an oval hole is a squashed cylinder, and a unit
    // sphere scaled 2x on X is the same move on the simplest primitive.
    const float scale[3] = {2.0f, 1.0f, 1.0f};
    REQUIRE(clay_layer_set_transform_nonuniform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f,
                                                scale) == CLAY_OK);
    CHECK(eval_one(d.doc, 2.0f, 0, 0) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_one(d.doc, 0, 1.0f, 0) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_one(d.doc, 3.0f, 0, 0) > 0.0f);
}

TEST_CASE("the two scale readers agree, and the uniform one refuses what it cannot say") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer);

    // A UNIFORM placement answers on both readers, and the per-axis one reports
    // (s, s, s) rather than hiding the factor somewhere the caller cannot see.
    REQUIRE(clay_layer_set_transform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f, 1.25f) ==
            CLAY_OK);
    float uniform = 0.0f;
    REQUIRE(clay_layer_node_transform(d.doc, layer, node, nullptr, nullptr, nullptr, &uniform) ==
            CLAY_OK);
    CHECK(uniform == doctest::Approx(1.25f));
    float axes[3] = {0, 0, 0};
    REQUIRE(clay_layer_node_transform_nonuniform(d.doc, layer, node, nullptr, nullptr, nullptr,
                                                 axes) == CLAY_OK);
    for (int i = 0; i < 3; ++i) CHECK(axes[i] == doctest::Approx(1.25f));

    // Squash it, and the uniform reader REFUSES rather than reporting one of
    // the three or some average of them. #317's lesson: a reader that cannot
    // express what is there must not answer, because a host doing
    // read-change-write would round the artist's squash away.
    const float squash[3] = {2.0f, 0.5f, 3.0f};
    REQUIRE(clay_layer_set_transform_nonuniform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f,
                                                squash) == CLAY_OK);
    uniform = -1.0f;
    CHECK(clay_layer_node_transform(d.doc, layer, node, nullptr, nullptr, nullptr, &uniform) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(uniform == -1.0f);  // nothing written

    // The per-axis reader always can, and what comes out goes straight back in.
    float position[3] = {0, 0, 0}, axis[3] = {0, 0, 0}, angle = 0.0f;
    REQUIRE(clay_layer_node_transform_nonuniform(d.doc, layer, node, position, axis, &angle,
                                                 axes) == CLAY_OK);
    CHECK(axes[0] == doctest::Approx(2.0f));
    CHECK(axes[1] == doctest::Approx(0.5f));
    CHECK(axes[2] == doctest::Approx(3.0f));
    REQUIRE(clay_layer_set_transform_nonuniform(d.doc, layer, node, position, axis, angle, axes) ==
            CLAY_OK);
    float again[3] = {0, 0, 0};
    REQUIRE(clay_layer_node_transform_nonuniform(d.doc, layer, node, nullptr, nullptr, nullptr,
                                                 again) == CLAY_OK);
    for (int i = 0; i < 3; ++i) CHECK(again[i] == doctest::Approx(axes[i]));
}

TEST_CASE("the uniform setter collapses a per-axis scale, and says so") {
    // Both setters write the WHOLE transform — this ABI does not do partial
    // updates — so the uniform one means "this node's scale is uniform s".
    // Pinned because the alternative (quietly keeping one component) is the
    // kind of thing a host would only discover from a wrong-looking model.
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer);
    const float squash[3] = {2.0f, 0.5f, 3.0f};
    REQUIRE(clay_layer_set_transform_nonuniform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f,
                                                squash) == CLAY_OK);

    REQUIRE(clay_layer_set_transform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f, 2.0f) ==
            CLAY_OK);
    float axes[3] = {0, 0, 0};
    REQUIRE(clay_layer_node_transform_nonuniform(d.doc, layer, node, nullptr, nullptr, nullptr,
                                                 axes) == CLAY_OK);
    for (int i = 0; i < 3; ++i) CHECK(axes[i] == doctest::Approx(2.0f));
    // ...and the uniform reader answers again, because there is again one
    // number to answer with.
    float uniform = 0.0f;
    REQUIRE(clay_layer_node_transform(d.doc, layer, node, nullptr, nullptr, nullptr, &uniform) ==
            CLAY_OK);
    CHECK(uniform == doctest::Approx(2.0f));
}

TEST_CASE("the item builder carries a per-axis scale, multiplying the uniform one") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);

    const float r = 1.0f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_scale(item, 2.0f) == CLAY_OK);
    const float axes_in[3] = {1.5f, 1.0f, 1.0f};
    REQUIRE(clay_item_set_scale_nonuniform(item, axes_in) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(d.doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    // The two multiply: 2 * 1.5 on X, 2 on the others.
    float axes[3] = {0, 0, 0};
    REQUIRE(clay_layer_node_transform_nonuniform(d.doc, layer, node, nullptr, nullptr, nullptr,
                                                 axes) == CLAY_OK);
    CHECK(axes[0] == doctest::Approx(3.0f));
    CHECK(axes[1] == doctest::Approx(2.0f));
    CHECK(eval_one(d.doc, 3.0f, 0, 0) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_one(d.doc, 0, 2.0f, 0) == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("a per-axis scale keeps its refusals typed") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    const clay_node_id node = add_sphere(d.doc, layer);

    // Zero has no inverse and would collapse the item onto a plane; a negative
    // component mirrors it, which the layer mirror already expresses and which
    // would flip a boolean's winding without saying so.
    const float zero[3] = {1.0f, 0.0f, 1.0f};
    const float negative[3] = {1.0f, -2.0f, 1.0f};
    CHECK(clay_layer_set_transform_nonuniform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f,
                                              zero) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_transform_nonuniform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f,
                                              negative) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_transform_nonuniform(d.doc, layer, node, kOrigin, kIdentityAxis, 0.0f,
                                              nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    // A refused edit leaves the node alone.
    float axes[3] = {0, 0, 0};
    REQUIRE(clay_layer_node_transform_nonuniform(d.doc, layer, node, nullptr, nullptr, nullptr,
                                                 axes) == CLAY_OK);
    for (int i = 0; i < 3; ++i) CHECK(axes[i] == doctest::Approx(1.0f));

    // A group has no transform of its own, on both calls.
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(d.doc, layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f,
                                 &group) == CLAY_OK);
    const float ok_scale[3] = {2.0f, 1.0f, 1.0f};
    CHECK(clay_layer_set_transform_nonuniform(d.doc, layer, group, kOrigin, kIdentityAxis, 0.0f,
                                              ok_scale) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_transform_nonuniform(d.doc, layer, group, nullptr, nullptr, nullptr,
                                               axes) == CLAY_ERROR_INVALID_ARGUMENT);

    // And the ordinary lookups.
    CHECK(clay_layer_node_transform_nonuniform(d.doc, layer + 77, node, nullptr, nullptr, nullptr,
                                               axes) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_node_transform_nonuniform(nullptr, layer, node, nullptr, nullptr, nullptr,
                                               axes) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &ok_scale[1], 1);
    REQUIRE(item != nullptr);
    CHECK(clay_item_set_scale_nonuniform(item, zero) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_scale_nonuniform(nullptr, ok_scale) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(item);
}

TEST_CASE("a per-axis mesh transform tilts no normal") {
    // The mesh arm has no field, so exactness never enters. What DOES is the
    // normals: under a squash a normal is no longer carried by the rotation
    // alone, and transforming it as a direction leaves every one of them off
    // the surface. The inverse transpose is the difference, and this is the
    // case that would catch its absence.
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "camada", &layer) == CLAY_OK);
    add_sphere(d.doc, layer);

    clay_mesh* source = nullptr;
    clay_mesh_params params;
    std::memset(&params, 0, sizeof params);
    params.struct_size = sizeof params;
    params.resolution = 48;
    REQUIRE(clay_document_mesh(d.doc, &params, &source) == CLAY_OK);

    const float scale[3] = {3.0f, 1.0f, 1.0f};
    clay_mesh* squashed = nullptr;
    REQUIRE(clay_mesh_transform_nonuniform(source, kOrigin, kIdentityAxis, 0.0f, scale,
                                           &squashed) == CLAY_OK);

    const size_t vertices = clay_mesh_vertex_count(squashed);
    REQUIRE(vertices > 0);
    const float* pos = clay_mesh_positions(squashed);
    const float* nrm = clay_mesh_normals(squashed);
    REQUIRE(pos != nullptr);
    REQUIRE(nrm != nullptr);

    // Every normal stays a unit vector, and — the part a rotation-only
    // transform gets wrong — it still points along the ELLIPSOID's gradient,
    // which for x^2/9 + y^2 + z^2 = 1 is (x/9, y, z) normalized.
    //
    // The threshold is measured, not guessed. Over these 32210 vertices the
    // worst agreement is 0.999999 through the inverse transpose and 0.865830
    // if the normals are merely rotated, so 0.999 separates the two by a wide
    // margin in both directions.
    int checked = 0;
    for (size_t i = 0; i < vertices; ++i) {
        const float* p = pos + i * 3;
        const float* n = nrm + i * 3;
        CHECK(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ==
              doctest::Approx(1.0f).epsilon(1e-3));
        float g[3] = {p[0] / 9.0f, p[1], p[2]};
        const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (len < 1e-4f) continue;
        const float dot = (g[0] * n[0] + g[1] * n[1] + g[2] * n[2]) / len;
        CHECK(dot > 0.999f);
        ++checked;
    }
    CHECK(checked > 1000);

    clay_mesh_destroy(squashed);
    clay_mesh_destroy(source);
}
