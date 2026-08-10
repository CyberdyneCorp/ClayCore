// The C ABI mesh-layer surface (c-abi spec: mesh layers across the ABI).
// Attaching, looking up, bounds, the borrowed handle's lifetime, and the two
// properties the change is really about: the geometry survives a save and
// reload verbatim, and clay_document_mesh is untouched by a mesh layer being
// there at all.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
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

clay_mesh_layer_desc layer_desc(const char* name) {
    clay_mesh_layer_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<std::uint32_t>(sizeof d);
    d.name = name;
    return d;
}

// A tetrahedron: four vertices, four faces, and no two coordinates alike, so a
// transposed or truncated buffer is visible rather than plausible.
clay_mesh* tetrahedron() {
    const float positions[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 3.0f};
    const std::uint32_t indices[12] = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 12, &m) == CLAY_OK);
    return m;
}

std::vector<float> positions_of(const clay_mesh* m) {
    const float* p = clay_mesh_positions(m);
    return std::vector<float>(p, p + clay_mesh_vertex_count(m) * 3);
}

std::vector<std::uint32_t> indices_of(const clay_mesh* m) {
    const std::uint32_t* i = clay_mesh_indices(m);
    return std::vector<std::uint32_t>(i, i + clay_mesh_index_count(m));
}

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// A document with something to evaluate, so "the tape is unchanged" is a
// claim about a document that has a tape.
void add_sphere(clay_document* doc) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc item;
    std::memset(&item, 0, sizeof item);
    item.struct_size = static_cast<std::uint32_t>(sizeof item);
    item.prim = CLAY_PRIM_SPHERE;
    item.params[0] = 0.5f;
    item.scale = 1.0f;
    item.rotation[3] = 1.0f;
    REQUIRE(clay_add_item(doc, layer, &item, nullptr) == CLAY_OK);
}

}  // namespace

TEST_CASE("c mesh layer: a host imports a model, keeps it, saves and reloads") {
    const std::string path = temp_path("clay_mesh_layer_roundtrip.clayspace");
    clay_mesh* source = tetrahedron();
    std::vector<float> expect_positions;
    std::vector<std::uint32_t> expect_indices;
    {
        Doc d;
        add_sphere(d.doc);
        clay_mesh_layer_desc desc = layer_desc("scan");
        clay_layer_id layer = 0;
        clay_mesh* borrowed = nullptr;
        REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, &borrowed) == CLAY_OK);
        REQUIRE(borrowed != nullptr);
        expect_positions = positions_of(borrowed);
        expect_indices = indices_of(borrowed);
        // Moving the layer does not move the stored vertices.
        const float pos[3] = {1.5f, 0.0f, -2.0f};
        const float axis[3] = {0.0f, 1.0f, 0.0f};
        REQUIRE(clay_document_set_layer_transform(d.doc, layer, pos, axis, 0.5f, 1.0f) ==
                CLAY_OK);
        CHECK(positions_of(borrowed) == expect_positions);
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }
    clay_mesh_destroy(source);

    clay_document* loaded = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &loaded) == CLAY_OK);
    clay_layer_id layer = 0;
    clay_mesh* back = nullptr;
    REQUIRE(clay_document_mesh_layer(loaded, "scan", &layer, &back) == CLAY_OK);
    CHECK(positions_of(back) == expect_positions);
    CHECK(indices_of(back) == expect_indices);
    clay_document_destroy(loaded);
    std::filesystem::remove(path);
}

TEST_CASE("c mesh layer: attach is undoable, and the geometry survives the undo") {
    Doc d;
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    clay_layer_id layer = 0;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, nullptr) == CLAY_OK);
    clay_mesh_destroy(source);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(clay_document_mesh_layer(d.doc, "scan", nullptr, nullptr) == CLAY_ERROR_NOT_FOUND);

    // Redo restores the layer under the same id, so the payload — which is
    // never erased on removal — is picked back up.
    std::int32_t redone = 0;
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(redone == 1);
    clay_mesh* back = nullptr;
    REQUIRE(clay_document_mesh_layer(d.doc, "scan", nullptr, &back) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(back) == 4);
}

TEST_CASE("c mesh layer: the attach budget refuses an oversized mesh") {
    Doc d;
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    desc.max_vertices = 3;  // the tetrahedron has four
    CHECK(clay_document_add_mesh_layer(d.doc, source, &desc, nullptr, nullptr) ==
          CLAY_ERROR_BUDGET_EXCEEDED);
    desc.max_vertices = 0;
    desc.max_triangles = 3;  // it has four faces
    CHECK(clay_document_add_mesh_layer(d.doc, source, &desc, nullptr, nullptr) ==
          CLAY_ERROR_BUDGET_EXCEEDED);
    // and the document is unchanged
    CHECK(clay_document_mesh_layer(d.doc, "scan", nullptr, nullptr) == CLAY_ERROR_NOT_FOUND);
    clay_mesh_destroy(source);
}

TEST_CASE("c mesh layer: the import scale is baked into the stored vertices") {
    Doc d;
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    desc.import_scale = 0.01f;  // millimetres to metres, the FBX case
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, nullptr, &borrowed) == CLAY_OK);
    clay_mesh_destroy(source);
    float lo[3] = {0, 0, 0};
    float hi[3] = {0, 0, 0};
    REQUIRE(clay_mesh_bounds(borrowed, lo, hi) == CLAY_OK);
    CHECK(hi[2] == doctest::Approx(0.03f));  // the z extent was 3
    CHECK(lo[0] == doctest::Approx(0.0f));
}

TEST_CASE("c mesh layer: a borrowed handle is not the caller's to free") {
    Doc d;
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, nullptr, &borrowed) == CLAY_OK);
    clay_mesh_destroy(source);

    clay_mesh_destroy(borrowed);  // a no-op: the document owns it
    clay_mesh* again = nullptr;
    REQUIRE(clay_document_mesh_layer(d.doc, "scan", nullptr, &again) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(again) == 4);
    CHECK(clay_mesh_index_count(again) == 12);
}

TEST_CASE("c mesh layer: a borrowed handle names the layer it belongs to") {
    Doc d;
    clay_mesh* source = tetrahedron();
    clay_layer_id owned_layer = 12345;
    CHECK(clay_mesh_layer(source, &owned_layer) == CLAY_ERROR_NOT_FOUND);
    CHECK(owned_layer == 12345);  // untouched

    clay_mesh_layer_desc desc = layer_desc("scan");
    clay_layer_id layer = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, &borrowed) == CLAY_OK);
    clay_mesh_destroy(source);
    clay_layer_id from_handle = 0;
    REQUIRE(clay_mesh_layer(borrowed, &from_handle) == CLAY_OK);
    CHECK(from_handle == layer);

    // Removing the layer leaves the payload — the inverse of a removal cannot
    // carry it — so the handle keeps resolving and the save filtering is what
    // makes the orphan harmless.
    REQUIRE(clay_document_remove_layer(d.doc, layer) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(borrowed) == 4);
    CHECK(clay_document_mesh_layer(d.doc, "scan", nullptr, nullptr) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("c mesh layer: not found, and the refusals") {
    Doc d;
    CHECK(clay_document_mesh_layer(d.doc, "nothing", nullptr, nullptr) == CLAY_ERROR_NOT_FOUND);
    add_sphere(d.doc);
    // an SDF layer by that name is still not a mesh layer
    CHECK(clay_document_mesh_layer(d.doc, "body", nullptr, nullptr) == CLAY_ERROR_NOT_FOUND);

    clay_mesh_layer_desc desc = layer_desc(nullptr);
    clay_mesh* source = tetrahedron();
    CHECK(clay_document_add_mesh_layer(d.doc, source, &desc, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_mesh_destroy(source);
}

TEST_CASE("c mesh layer: bounds come off the mesh, not off clay_layer_bounds") {
    Doc d;
    clay_mesh* source = tetrahedron();
    float lo[3] = {0, 0, 0};
    float hi[3] = {0, 0, 0};
    REQUIRE(clay_mesh_bounds(source, lo, hi) == CLAY_OK);
    CHECK(lo[0] == doctest::Approx(0.0f));
    CHECK(hi[0] == doctest::Approx(1.0f));
    CHECK(hi[1] == doctest::Approx(2.0f));
    CHECK(hi[2] == doctest::Approx(3.0f));

    clay_mesh_layer_desc desc = layer_desc("scan");
    clay_layer_id layer = 0;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, nullptr) == CLAY_OK);
    clay_mesh_destroy(source);
    // clay_layer_bounds is derived from SDF shapes and is deliberately left
    // meaning what it always meant: a mesh layer shows it nothing.
    float bmin[3] = {0, 0, 0};
    float bmax[3] = {0, 0, 0};
    std::int32_t has_bounds = 1;
    REQUIRE(clay_layer_bounds(d.doc, layer, bmin, bmax, &has_bounds) == CLAY_OK);
    CHECK(has_bounds == 0);
}

TEST_CASE("c mesh layer: clay_document_mesh is untouched by one") {
    std::vector<float> plain;
    {
        Doc d;
        add_sphere(d.doc);
        clay_mesh_params p;
        std::memset(&p, 0, sizeof p);
        p.struct_size = static_cast<std::uint32_t>(sizeof p);
        p.resolution = 32;
        clay_mesh* m = nullptr;
        REQUIRE(clay_document_mesh(d.doc, &p, &m) == CLAY_OK);
        plain = positions_of(m);
        clay_mesh_destroy(m);
    }
    Doc d;
    add_sphere(d.doc);
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, nullptr, nullptr) == CLAY_OK);
    clay_mesh_destroy(source);

    clay_mesh_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.resolution = 32;
    clay_mesh* m = nullptr;
    REQUIRE(clay_document_mesh(d.doc, &p, &m) == CLAY_OK);
    CHECK(positions_of(m) == plain);  // bit-identical, not merely similar
    clay_mesh_destroy(m);
}
