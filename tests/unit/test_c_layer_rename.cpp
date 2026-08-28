// Renaming a layer across the C ABI (#92, c-abi spec: a layer can be renamed).
// A layer was named by whichever call created it and nothing could change it,
// so a host that let the artist rename a layer kept that name beside the
// document and lost it on the next save. clay_layer_name (#69) made the loss
// visible rather than caused it: a reopened document reports the creation name
// and looks correct doing so. The regression case below is exactly that —
// rename, save, reload, read the name back.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
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

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

std::string name_of(const clay_document* doc, clay_layer_id layer) {
    size_t size = 0;
    REQUIRE(clay_layer_name(doc, layer, nullptr, &size) == CLAY_OK);
    std::vector<char> buffer(size);
    REQUIRE(clay_layer_name(doc, layer, buffer.data(), &size) == CLAY_OK);
    return std::string(buffer.data());
}

// A document a host reloads has content; the sphere is irrelevant to the name
// but keeps the saved layer from being empty.
void add_sphere(clay_document* doc, clay_layer_id layer) {
    clay_item_desc item;
    std::memset(&item, 0, sizeof item);
    item.struct_size = sizeof item;
    item.prim = CLAY_PRIM_SPHERE;
    item.params[0] = 0.5f;
    item.rotation[3] = 1.0f;
    item.scale = 1.0f;
    item.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &item, &node) == CLAY_OK);
}

clay_layer_id add_mesh_layer(clay_document* doc, const char* name) {
    const float positions[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 3.0f};
    const std::uint32_t indices[12] = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};
    clay_mesh* tetra = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 12, &tetra) == CLAY_OK);
    clay_mesh_layer_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.name = name;
    clay_layer_id layer = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(doc, tetra, &desc, &layer, &borrowed) == CLAY_OK);
    clay_mesh_destroy(tetra);
    return layer;
}

// A second mesh layer whose geometry is a different SIZE from the tetrahedron's,
// so a lookup that reached the wrong one of two same-named layers is visible in
// the vertex count rather than only in the coordinates.
clay_layer_id add_triangle_layer(clay_document* doc, const char* name) {
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::uint32_t indices[3] = {0, 1, 2};
    clay_mesh* tri = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 3, indices, 3, &tri) == CLAY_OK);
    clay_mesh_layer_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.name = name;
    clay_layer_id layer = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(doc, tri, &desc, &layer, &borrowed) == CLAY_OK);
    clay_mesh_destroy(tri);
    return layer;
}

}  // namespace

// The regression: on 0.29.1 the rename lived only in the host, so the reloaded
// document answered "Base" — the creation name — and a host that trusted
// clay_layer_name showed the artist a name they had changed and saved.
TEST_CASE("a rename survives a save and reload, in every representation") {
    const std::string path = temp_path("c_layer_rename_roundtrip.clayspace");
    clay_layer_id sdf = 0, voxel = 0, mesh = 0;
    {
        Doc d;
        REQUIRE(clay_add_sdf_layer(d.doc, "Base", &sdf) == CLAY_OK);
        add_sphere(d.doc, sdf);
        clay_voxel_grid* grid = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d.doc, "Grade", 0.05f, &voxel, &grid) == CLAY_OK);
        mesh = add_mesh_layer(d.doc, "Malha");

        REQUIRE(clay_document_set_layer_name(d.doc, sdf, "Torso") == CLAY_OK);
        REQUIRE(clay_document_set_layer_name(d.doc, voxel, "Argila") == CLAY_OK);
        REQUIRE(clay_document_set_layer_name(d.doc, mesh, "Referencia") == CLAY_OK);
        // In memory before the save, so a failure below is a persistence
        // failure rather than a setter that never applied.
        CHECK(name_of(d.doc, sdf) == "Torso");
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    CHECK(name_of(back, sdf) == "Torso");
    CHECK(name_of(back, voxel) == "Argila");
    CHECK(name_of(back, mesh) == "Referencia");
    clay_document_destroy(back);
    std::filesystem::remove(path);
}

TEST_CASE("a rename is one undo step, and redo puts it back") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Base", &layer) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);

    REQUIRE(clay_document_set_layer_name(d.doc, layer, "Torso") == CLAY_OK);
    REQUIRE(clay_document_set_layer_name(d.doc, layer, "Peito") == CLAY_OK);
    CHECK(name_of(d.doc, layer) == "Peito");

    // One step per rename: the second undo reaches the creation name rather
    // than stopping at it.
    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(name_of(d.doc, layer) == "Torso");
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(name_of(d.doc, layer) == "Base");

    int32_t redone = 0;
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(redone == 1);
    CHECK(name_of(d.doc, layer) == "Torso");
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(name_of(d.doc, layer) == "Peito");
}

TEST_CASE("renaming a voxel layer moves the name its grid is looked up by") {
    Doc d;
    clay_layer_id voxel = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(d.doc, "Grade", 0.05f, &voxel, &grid) == CLAY_OK);
    REQUIRE(clay_document_set_layer_name(d.doc, voxel, "Argila") == CLAY_OK);

    clay_layer_id found = 0;
    clay_voxel_grid* by_name = nullptr;
    REQUIRE(clay_document_voxel_layer(d.doc, "Argila", &found, &by_name) == CLAY_OK);
    CHECK(found == voxel);
    CHECK(by_name != nullptr);
    // The old name looks up nothing: the name IS the key, which is why the
    // header tells a host to hold the id when the lookup must outlive a rename.
    CHECK(clay_document_voxel_layer(d.doc, "Grade", &found, &by_name) == CLAY_ERROR_NOT_FOUND);

    // The mesh lookup keys on the name the same way and follows it the same way.
    clay_layer_id mesh = add_mesh_layer(d.doc, "Malha");
    REQUIRE(clay_document_set_layer_name(d.doc, mesh, "Referencia") == CLAY_OK);
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer(d.doc, "Referencia", &found, &borrowed) == CLAY_OK);
    CHECK(found == mesh);
    CHECK(clay_document_mesh_layer(d.doc, "Malha", &found, &borrowed) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("a duplicate name is allowed, and the lookup answers the first in stack order") {
    Doc d;
    // The create calls accept a name already in use, so the setter does too —
    // refusing here would buy a uniqueness the document never had.
    clay_layer_id first = 0, second = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(d.doc, "Argila", 0.05f, &first, &grid) == CLAY_OK);
    REQUIRE(clay_document_add_voxel_layer(d.doc, "Rascunho", 0.05f, &second, &grid) == CLAY_OK);
    REQUIRE(clay_document_set_layer_name(d.doc, second, "Argila") == CLAY_OK);
    CHECK(name_of(d.doc, first) == "Argila");
    CHECK(name_of(d.doc, second) == "Argila");

    // Shadowed, not rebound: the earlier layer in stack order still answers.
    clay_layer_id found = 0;
    REQUIRE(clay_document_voxel_layer(d.doc, "Argila", &found, &grid) == CLAY_OK);
    CHECK(found == first);

    // And the shadowing is positional, not creation-ordered: move the renamed
    // layer to the front and it becomes the one the lookup finds.
    REQUIRE(clay_document_move_layer(d.doc, second, 0) == CLAY_OK);
    REQUIRE(clay_document_voxel_layer(d.doc, "Argila", &found, &grid) == CLAY_OK);
    CHECK(found == second);
}

TEST_CASE("the rename refuses what it cannot name") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Base", &layer) == CLAY_OK);

    CHECK(clay_document_set_layer_name(nullptr, layer, "Torso") == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_name(d.doc, layer, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    // An emptied text field must not replace the only name the document has.
    CHECK(clay_document_set_layer_name(d.doc, layer, "") == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_name(d.doc, layer + 9999, "Torso") == CLAY_ERROR_NOT_FOUND);
    // Every refusal leaves the name alone.
    CHECK(name_of(d.doc, layer) == "Base");
}

TEST_CASE("a protected layer refuses the rename, like every other layer edit") {
    Doc d;
    clay_layer_id locked = 0, ghosted = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Pronta", &locked) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(d.doc, "Fantasma", &ghosted) == CLAY_OK);
    REQUIRE(clay_document_set_layer_protection(d.doc, locked, 0, 1) == CLAY_OK);
    REQUIRE(clay_document_set_layer_protection(d.doc, ghosted, 1, 0) == CLAY_OK);

    CHECK(clay_document_set_layer_name(d.doc, locked, "Torso") == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_name(d.doc, ghosted, "Torso") == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(name_of(d.doc, locked) == "Pronta");
    CHECK(name_of(d.doc, ghosted) == "Fantasma");

    // Releasing the protection is not itself an edit of the layer, so the
    // rename becomes possible again — locking is never one-way.
    REQUIRE(clay_document_set_layer_protection(d.doc, locked, 0, 0) == CLAY_OK);
    REQUIRE(clay_document_set_layer_name(d.doc, locked, "Torso") == CLAY_OK);
    CHECK(name_of(d.doc, locked) == "Torso");
}

TEST_CASE("there is no name length limit, and the size query reports what it took") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Base", &layer) == CLAY_OK);
    // Length-prefixed in the layer record and size-queried on the way out, so
    // a long name costs the reader a bigger buffer rather than a truncation.
    const std::string quilo(4096, 'q');
    REQUIRE(clay_document_set_layer_name(d.doc, layer, quilo.c_str()) == CLAY_OK);
    size_t size = 0;
    REQUIRE(clay_layer_name(d.doc, layer, nullptr, &size) == CLAY_OK);
    CHECK(size == quilo.size() + 1);
    CHECK(name_of(d.doc, layer) == quilo);

    const std::string path = temp_path("c_layer_rename_long.clayspace");
    REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    CHECK(name_of(back, layer) == quilo);
    clay_document_destroy(back);
    std::filesystem::remove(path);
}

// UTF-8 is bytes to the ABI and to the format alike: a name with multi-byte
// characters is not truncated mid-character, and the size query counts bytes.
TEST_CASE("a UTF-8 name round-trips byte for byte") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Base", &layer) == CLAY_OK);
    const std::string name = "Cabe\xc3\xa7"
                             "a \xe2\x80\x94 detalhe";
    REQUIRE(clay_document_set_layer_name(d.doc, layer, name.c_str()) == CLAY_OK);
    size_t size = 0;
    REQUIRE(clay_layer_name(d.doc, layer, nullptr, &size) == CLAY_OK);
    CHECK(size == name.size() + 1);
    CHECK(name_of(d.doc, layer) == name);
}

// #365. The advice above clay_document_set_layer_name — hold the id, names are
// not a key — could not be followed for the two representations that have a
// payload: the only route back to a grid or to a mesh went through the name,
// and answered with the first layer carrying it. So two layers sharing a name
// shadowed one another SILENTLY, since the lookup succeeded, and a host's only
// defence was to forbid duplicate names on voxel layers.
TEST_CASE("two voxel layers sharing a name are told apart by their ids") {
    const std::string path = temp_path("c_layer_by_id_voxel.clayspace");
    clay_layer_id first = 0, second = 0;
    const int32_t cell_a[3] = {1, 0, 0};
    const int32_t cell_b[3] = {2, 0, 0};
    {
        Doc d;
        clay_voxel_grid* grid = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d.doc, "Esfera", 0.05f, &first, &grid) == CLAY_OK);
        REQUIRE(clay_voxel_set(grid, cell_a, 1) == CLAY_OK);
        REQUIRE(clay_document_add_voxel_layer(d.doc, "Esfera", 0.05f, &second, &grid) == CLAY_OK);
        REQUIRE(clay_voxel_set(grid, cell_b, 2) == CLAY_OK);

        // The by-name lookup reaches the first in stack order and nothing else:
        // that is the shadowing, and it is not a defect being fixed here.
        clay_layer_id found = 0;
        REQUIRE(clay_document_voxel_layer(d.doc, "Esfera", &found, &grid) == CLAY_OK);
        CHECK(found == first);

        // The ids reach their own layers, which is the whole ask.
        clay_voxel_grid* a = nullptr;
        clay_voxel_grid* b = nullptr;
        REQUIRE(clay_document_voxel_layer_by_id(d.doc, first, &a) == CLAY_OK);
        REQUIRE(clay_document_voxel_layer_by_id(d.doc, second, &b) == CLAY_OK);
        CHECK(a != b);
        int32_t read = 0;
        REQUIRE(clay_voxel_get(a, cell_a, &read) == CLAY_OK);
        CHECK(read == 1);
        REQUIRE(clay_voxel_get(a, cell_b, &read) == CLAY_OK);
        CHECK(read == 0);  // the second layer's cell is not in the first's grid
        REQUIRE(clay_voxel_get(b, cell_b, &read) == CLAY_OK);
        CHECK(read == 2);
        REQUIRE(clay_voxel_get(b, cell_a, &read) == CLAY_OK);
        CHECK(read == 0);

        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    // Ids are stable across a save and load and names are not, so the round
    // trip is where holding an id has to pay off.
    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    clay_voxel_grid* a = nullptr;
    clay_voxel_grid* b = nullptr;
    REQUIRE(clay_document_voxel_layer_by_id(back, first, &a) == CLAY_OK);
    REQUIRE(clay_document_voxel_layer_by_id(back, second, &b) == CLAY_OK);
    int32_t read = 0;
    REQUIRE(clay_voxel_get(a, cell_a, &read) == CLAY_OK);
    CHECK(read == 1);
    REQUIRE(clay_voxel_get(b, cell_b, &read) == CLAY_OK);
    CHECK(read == 2);

    // And a rename moves the name without moving what the id reaches — the
    // property the header promised and could not deliver.
    REQUIRE(clay_document_set_layer_name(back, second, "Cabeca") == CLAY_OK);
    REQUIRE(clay_document_voxel_layer_by_id(back, second, &b) == CLAY_OK);
    REQUIRE(clay_voxel_get(b, cell_b, &read) == CLAY_OK);
    CHECK(read == 2);
    clay_document_destroy(back);
    std::filesystem::remove(path);
}

TEST_CASE("two mesh layers sharing a name are told apart by their ids") {
    const std::string path = temp_path("c_layer_by_id_mesh.clayspace");
    clay_layer_id first = 0, second = 0;
    {
        Doc d;
        // Four vertices against three, so "the right layer" is checked rather
        // than "a layer".
        first = add_mesh_layer(d.doc, "Malha");
        second = add_triangle_layer(d.doc, "Malha");

        clay_layer_id found = 0;
        clay_mesh* by_name = nullptr;
        REQUIRE(clay_document_mesh_layer(d.doc, "Malha", &found, &by_name) == CLAY_OK);
        CHECK(found == first);

        clay_mesh* a = nullptr;
        clay_mesh* b = nullptr;
        REQUIRE(clay_document_mesh_layer_by_id(d.doc, first, &a) == CLAY_OK);
        REQUIRE(clay_document_mesh_layer_by_id(d.doc, second, &b) == CLAY_OK);
        CHECK(a != b);
        CHECK(clay_mesh_vertex_count(a) == 4);
        CHECK(clay_mesh_vertex_count(b) == 3);
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);
    clay_mesh* a = nullptr;
    clay_mesh* b = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(back, first, &a) == CLAY_OK);
    REQUIRE(clay_document_mesh_layer_by_id(back, second, &b) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(a) == 4);
    CHECK(clay_mesh_vertex_count(b) == 3);

    REQUIRE(clay_document_set_layer_name(back, first, "Referencia") == CLAY_OK);
    REQUIRE(clay_document_mesh_layer_by_id(back, first, &a) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(a) == 4);
    clay_document_destroy(back);
    std::filesystem::remove(path);
}
