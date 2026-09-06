// A document carries a hierarchy, across the C ABI (persist-a-multires-hierarchy).
//
// The host case this exists for: a hierarchy row is TWO objects -- a mesh layer
// holding the cage and a clay_multires beside it -- and because the engine
// reported the layer as an ordinary MESH layer, a host's own side-car file was
// the only record that the row had ever been a hierarchy. What is asserted here
// is that the document is now that record: it says which rows carry one, hands
// the hierarchy back after a load, and keeps the two halves of the row apart
// rather than reconciling them.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay.h"

namespace {

void plane(int n, float half, std::vector<float>* positions, std::vector<uint32_t>* indices) {
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            positions->push_back(-half + step * static_cast<float>(x));
            positions->push_back(0.0f);
            positions->push_back(-half + step * static_cast<float>(z));
        }
    const uint32_t stride = static_cast<uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const uint32_t a = static_cast<uint32_t>(z) * stride + static_cast<uint32_t>(x);
            const uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            indices->insert(indices->end(), {a, b, c, a, c, d});
        }
}

clay_mesh* make_mesh(int n = 4) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(n, 2.0f, &positions, &indices);
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &m) == CLAY_OK);
    return m;
}

clay_multires* make_hierarchy(const clay_mesh* mesh, uint32_t levels) {
    clay_multires_desc desc{};
    desc.struct_size = sizeof(desc);
    REQUIRE(clay_multires_defaults(&desc) == CLAY_OK);
    clay_multires* s = nullptr;
    int32_t err = -1;
    REQUIRE(clay_multires_from_mesh(mesh, &desc, &s, &err) == CLAY_OK);
    for (uint32_t i = 0; i < levels; ++i) REQUIRE(clay_multires_add_level(s, nullptr, &err) == CLAY_OK);
    return s;
}

clay_layer_id add_cage(clay_document* doc, const clay_mesh* mesh, const char* name) {
    clay_mesh_layer_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.name = name;
    clay_layer_id id = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(doc, mesh, &desc, &id, &borrowed) == CLAY_OK);
    return id;
}

// A document with one mesh layer whose cage carries a hierarchy.
struct Doc {
    clay_document* doc = clay_document_create();
    clay_mesh* mesh = make_mesh();
    clay_layer_id layer = 0;
    clay_multires* handle = nullptr;  // borrowed after the take

    explicit Doc(uint32_t levels = 2) {
        layer = add_cage(doc, mesh, "Cage");
        handle = make_hierarchy(mesh, levels);
        REQUIRE(clay_layer_take_multires(doc, layer, handle) == CLAY_OK);
    }
    ~Doc() {
        clay_multires_destroy(handle);
        clay_mesh_destroy(mesh);
        clay_document_destroy(doc);
    }
};

}  // namespace

TEST_CASE("a document says which rows carry a hierarchy") {
    Doc d;
    int32_t present = -1;
    REQUIRE(clay_layer_multires_present(d.doc, d.layer, &present) == CLAY_OK);
    CHECK(present == 1);

    // A second mesh layer with nothing on it: present is 0, and that is an
    // ordinary answer rather than a failure.
    clay_layer_id plain = 0;
    plain = add_cage(d.doc, d.mesh, "Plain");
    present = -1;
    REQUIRE(clay_layer_multires_present(d.doc, plain, &present) == CLAY_OK);
    CHECK(present == 0);
}

TEST_CASE("not a mesh layer and no such layer are told apart") {
    Doc d;
    int32_t present = -1;
    clay_layer_id sdf = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Sdf", &sdf) == CLAY_OK);
    // An SDF layer cannot take the operation: INVALID_ARGUMENT, so that
    // NOT_FOUND keeps meaning "no layer carries this id" alone.
    CHECK(clay_layer_multires_present(d.doc, sdf, &present) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_multires_present(d.doc, 9999, &present) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("taking a hierarchy leaves the handle usable, pointing at the document's") {
    Doc d;
    // The handle was owned before the take and is borrowed after it, and the
    // same pointer still answers -- which is the whole reason the take repoints
    // it rather than leaving it moved-from.
    CHECK(clay_multires_level_count(d.handle) == 3u);
}

TEST_CASE("a borrowed handle survives its own destroy; the document keeps the hierarchy") {
    Doc d;
    clay_multires* borrowed = nullptr;
    REQUIRE(clay_layer_multires(d.doc, d.layer, &borrowed) == CLAY_OK);
    CHECK(clay_multires_level_count(borrowed) == 3u);
    clay_multires_destroy(borrowed);  // frees the handle, not the hierarchy

    int32_t present = -1;
    REQUIRE(clay_layer_multires_present(d.doc, d.layer, &present) == CLAY_OK);
    CHECK(present == 1);
}

TEST_CASE("removing the hierarchy makes a borrowed handle answer NOT_FOUND, not dangle") {
    Doc d;
    clay_multires* borrowed = nullptr;
    REQUIRE(clay_layer_multires(d.doc, d.layer, &borrowed) == CLAY_OK);
    REQUIRE(clay_layer_remove_multires(d.doc, d.layer) == CLAY_OK);

    // The stale handle is answerable rather than undefined.
    CHECK(clay_multires_level_count(borrowed) == 0u);
    uint32_t level = 0;
    CHECK(clay_multires_sculpt_level(borrowed, &level) == CLAY_ERROR_NOT_FOUND);
    clay_multires_destroy(borrowed);

    CHECK(clay_layer_remove_multires(d.doc, d.layer) == CLAY_ERROR_NOT_FOUND);
    // The cage stays: removing a hierarchy is not a reason to remove the mesh a
    // host is still drawing.
    int32_t present = -1;
    REQUIRE(clay_layer_multires_present(d.doc, d.layer, &present) == CLAY_OK);
    CHECK(present == 0);
}

TEST_CASE("replacing is refused, because the levels it would drop are unrecoverable") {
    Doc d;
    clay_multires* second = make_hierarchy(d.mesh, 1);
    CHECK(clay_layer_take_multires(d.doc, d.layer, second) == CLAY_ERROR_INVALID_ARGUMENT);
    // Remove first, then it is allowed -- the destruction is a call a host makes
    // on purpose rather than a side effect of an attach.
    REQUIRE(clay_layer_remove_multires(d.doc, d.layer) == CLAY_OK);
    CHECK(clay_layer_take_multires(d.doc, d.layer, second) == CLAY_OK);
    clay_multires_destroy(second);
}

TEST_CASE("a borrowed source has nothing to move") {
    Doc d;
    clay_layer_id other = 0;
    other = add_cage(d.doc, d.mesh, "Other");
    clay_multires* borrowed = nullptr;
    REQUIRE(clay_layer_multires(d.doc, d.layer, &borrowed) == CLAY_OK);
    CHECK(clay_layer_take_multires(d.doc, other, borrowed) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_multires_destroy(borrowed);
}

TEST_CASE("the cage and the hierarchy's base are compared, not reconciled") {
    Doc d;
    int32_t matches = -1;
    REQUIRE(clay_layer_multires_matches_cage(d.doc, d.layer, &matches) == CLAY_OK);
    CHECK(matches == 1);

    // A layer carrying no hierarchy has nothing to compare.
    clay_layer_id plain = 0;
    plain = add_cage(d.doc, d.mesh, "Plain");
    CHECK(clay_layer_multires_matches_cage(d.doc, plain, &matches) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("a hierarchy survives a save and a load, and is reachable afterwards") {
    std::vector<uint8_t> bytes;
    {
        Doc d;
        clay_blob* blob = nullptr;
        REQUIRE(clay_document_save_memory(d.doc, &blob) == CLAY_OK);
        const uint8_t* data = clay_blob_data(blob);
        const size_t size = clay_blob_size(blob);
        bytes.assign(data, data + size);
        clay_blob_destroy(blob);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(bytes.data(), bytes.size(), &back) == CLAY_OK);
    // Walk the rows the way a host building an outliner would, with no side-car
    // to consult -- which is the whole point.
    size_t count = 0;
    REQUIRE(clay_document_layer_count(back, &count) == CLAY_OK);
    REQUIRE(count >= 1);
    clay_layer_id id = 0;
    REQUIRE(clay_document_layer_at(back, 0, &id) == CLAY_OK);
    int32_t present = -1;
    REQUIRE(clay_layer_multires_present(back, id, &present) == CLAY_OK);
    CHECK(present == 1);

    clay_multires* loaded = nullptr;
    REQUIRE(clay_layer_multires(back, id, &loaded) == CLAY_OK);
    CHECK(clay_multires_level_count(loaded) == 3u);
    clay_multires_destroy(loaded);
    clay_document_destroy(back);
}
