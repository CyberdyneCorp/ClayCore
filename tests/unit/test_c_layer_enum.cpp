// The C ABI layer-discovery surface (#69, c-abi spec: a host can discover a
// document's layers). Count-then-index enumeration in stack order, the
// clay_layer_info descriptor, and the name query — pinned across the one path
// that made them necessary: a save and reload, with a reorder and a removal
// in the history, so id order and stack order disagree and the id space has a
// gap. Before this surface existed a host probed ids against
// clay_layer_bounds and a reopened document could EVALUATE differently from
// the one saved, because stack order was unrecoverable.

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
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// One sphere, so an SDF layer has content (irrelevant to enumeration, but a
// document a host reloads has content, and the probe this replaces needed it).
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

clay_layer_info info_of(const clay_document* doc, clay_layer_id layer) {
    clay_layer_info info;
    std::memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(clay_document_layer_info(doc, layer, &info) == CLAY_OK);
    return info;
}

std::string name_of(const clay_document* doc, clay_layer_id layer) {
    size_t size = 0;
    REQUIRE(clay_layer_name(doc, layer, nullptr, &size) == CLAY_OK);
    std::vector<char> buffer(size);
    REQUIRE(clay_layer_name(doc, layer, buffer.data(), &size) == CLAY_OK);
    REQUIRE(size == buffer.size());
    return std::string(buffer.data());
}

std::vector<clay_layer_id> enumerate(const clay_document* doc) {
    size_t count = 0;
    REQUIRE(clay_document_layer_count(doc, &count) == CLAY_OK);
    std::vector<clay_layer_id> ids;
    for (size_t i = 0; i < count; ++i) {
        clay_layer_id id = 0;
        REQUIRE(clay_document_layer_at(doc, i, &id) == CLAY_OK);
        ids.push_back(id);
    }
    return ids;
}

}  // namespace

TEST_CASE("an empty document counts zero layers") {
    Doc d;
    size_t count = 99;
    REQUIRE(clay_document_layer_count(d.doc, &count) == CLAY_OK);
    CHECK(count == 0);
    clay_layer_id id = 0;
    CHECK(clay_document_layer_at(d.doc, 0, &id) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("enumeration is in stack order and follows a move") {
    Doc d;
    clay_layer_id a = 0, b = 0, c = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "a", &a) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(d.doc, "b", &b) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(d.doc, "c", &c) == CLAY_OK);
    CHECK(enumerate(d.doc) == std::vector<clay_layer_id>{a, b, c});
    CHECK(info_of(d.doc, b).stack_index == 1);

    REQUIRE(clay_document_move_layer(d.doc, c, 0) == CLAY_OK);
    CHECK(enumerate(d.doc) == std::vector<clay_layer_id>{c, a, b});
    CHECK(info_of(d.doc, c).stack_index == 0);
    CHECK(info_of(d.doc, a).stack_index == 1);
    CHECK(info_of(d.doc, b).stack_index == 2);

    // one past the end is not found, exactly at the count boundary
    clay_layer_id id = 0;
    CHECK(clay_document_layer_at(d.doc, 3, &id) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("every discoverable field survives a save and reload") {
    // The issue's scenario plus everything settable: four layers across the
    // representations, a hide, a lock, a ghost, a reorder, then a removal so
    // the id space has the gap the old probe guessed at.
    const std::string path = temp_path("c_layer_enum_roundtrip.clayspace");
    clay_layer_id forma = 0, detalhe = 0, descartada = 0, terceira = 0, malha = 0;
    {
        Doc d;
        REQUIRE(clay_add_sdf_layer(d.doc, "Forma", &forma) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d.doc, "Detalhe", &detalhe) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d.doc, "Descartada", &descartada) == CLAY_OK);
        clay_voxel_grid* grid = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d.doc, "Terceira", 0.05f, &terceira, &grid) ==
                CLAY_OK);
        add_sphere(d.doc, forma);
        add_sphere(d.doc, detalhe);

        const float positions[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                     0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 3.0f};
        const std::uint32_t indices[12] = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};
        clay_mesh* tetra = nullptr;
        REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 12, &tetra) == CLAY_OK);
        clay_mesh_layer_desc mesh_desc;
        std::memset(&mesh_desc, 0, sizeof mesh_desc);
        mesh_desc.struct_size = sizeof mesh_desc;
        mesh_desc.name = "Malha";
        clay_mesh* borrowed = nullptr;
        REQUIRE(clay_document_add_mesh_layer(d.doc, tetra, &mesh_desc, &malha, &borrowed) ==
                CLAY_OK);
        clay_mesh_destroy(tetra);

        REQUIRE(clay_document_set_layer_visible(d.doc, detalhe, 0) == CLAY_OK);
        REQUIRE(clay_document_set_layer_protection(d.doc, forma, 0, 1) == CLAY_OK);
        // move before ghosting: a protected layer refuses edits, the move
        // included
        REQUIRE(clay_document_move_layer(d.doc, terceira, 0) == CLAY_OK);
        REQUIRE(clay_document_set_layer_protection(d.doc, terceira, 1, 0) == CLAY_OK);
        REQUIRE(clay_document_remove_layer(d.doc, descartada) == CLAY_OK);
        REQUIRE(clay_document_save(d.doc, path.c_str()) == CLAY_OK);
    }

    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path.c_str(), &back) == CLAY_OK);

    // The set AND the order, in one pair of calls per layer — no probe, no
    // gap constant. The removed id is simply absent.
    CHECK(enumerate(back) == std::vector<clay_layer_id>{terceira, forma, detalhe, malha});

    clay_layer_info info = info_of(back, terceira);
    CHECK(info.id == terceira);
    CHECK(info.representation == CLAY_LAYER_VOXEL);
    CHECK(info.stack_index == 0);
    CHECK(info.visible == 1);
    CHECK(info.ghost == 1);
    CHECK(info.locked == 0);
    CHECK(name_of(back, terceira) == "Terceira");

    info = info_of(back, forma);
    CHECK(info.representation == CLAY_LAYER_SDF);
    CHECK(info.stack_index == 1);
    CHECK(info.visible == 1);
    CHECK(info.ghost == 0);
    CHECK(info.locked == 1);
    CHECK(name_of(back, forma) == "Forma");

    // Hidden — and it still answers: reading is not editing.
    info = info_of(back, detalhe);
    CHECK(info.representation == CLAY_LAYER_SDF);
    CHECK(info.stack_index == 2);
    CHECK(info.visible == 0);
    CHECK(name_of(back, detalhe) == "Detalhe");

    info = info_of(back, malha);
    CHECK(info.representation == CLAY_LAYER_MESH);
    CHECK(info.stack_index == 3);
    CHECK(name_of(back, malha) == "Malha");

    // The removed layer's id answers nothing, which is what makes the gap
    // visible to a host instead of guessed at.
    clay_layer_info gone;
    std::memset(&gone, 0, sizeof gone);
    gone.struct_size = sizeof gone;
    CHECK(clay_document_layer_info(back, descartada, &gone) == CLAY_ERROR_NOT_FOUND);
    size_t size = 0;
    CHECK(clay_layer_name(back, descartada, nullptr, &size) == CLAY_ERROR_NOT_FOUND);

    clay_document_destroy(back);
    std::filesystem::remove(path);
}

TEST_CASE("the info descriptor enforces struct_size and the name query its buffer") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Camada", &layer) == CLAY_OK);

    // struct_size is required; zero is a 0.1.0-shaped caller, not a default.
    clay_layer_info info;
    std::memset(&info, 0, sizeof info);
    CHECK(clay_document_layer_info(d.doc, layer, &info) == CLAY_ERROR_INVALID_ARGUMENT);

    // Size query: NUL included, short buffer refused with the need reported
    // and nothing written.
    size_t size = 0;
    REQUIRE(clay_layer_name(d.doc, layer, nullptr, &size) == CLAY_OK);
    CHECK(size == 7);  // "Camada" + NUL
    char tiny[3] = {'x', 'x', 'x'};
    size_t tiny_size = sizeof tiny;
    CHECK(clay_layer_name(d.doc, layer, tiny, &tiny_size) == CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(tiny_size == 7);
    CHECK(tiny[0] == 'x');

    char exact[7];
    size_t exact_size = sizeof exact;
    REQUIRE(clay_layer_name(d.doc, layer, exact, &exact_size) == CLAY_OK);
    CHECK(std::string(exact) == "Camada");
}
