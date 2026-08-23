#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "clay.h"

// Serializing without a file (c-abi spec: serialized bytes cross the ABI;
// file-io spec: every format is reachable without a filesystem).
//
// Every format has always been implemented against buffers — the *_file entry
// points are wrappers over them — and only the wrappers crossed. A host whose
// documents arrive from a document provider, a network or its own container
// had to round-trip through a temporary file to use this library at all.
//
// The test that matters is the EQUIVALENCE one: memory and file must produce
// identical bytes, because that is what makes this plumbing rather than a
// second serializer to keep in step.

namespace {

struct Blob {
    clay_blob* b = nullptr;
    ~Blob() { clay_blob_destroy(b); }
    Blob() = default;
    Blob(const Blob&) = delete;
    Blob& operator=(const Blob&) = delete;
    std::vector<std::uint8_t> bytes() const {
        const std::uint8_t* p = clay_blob_data(b);
        const std::size_t n = clay_blob_size(b);
        return p ? std::vector<std::uint8_t>(p, p + n) : std::vector<std::uint8_t>{};
    }
};

struct MeshHandle {
    clay_mesh* m = nullptr;
    ~MeshHandle() { clay_mesh_destroy(m); }
    MeshHandle() = default;
    MeshHandle(const MeshHandle&) = delete;
    MeshHandle& operator=(const MeshHandle&) = delete;
};

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
        const float r = 0.5f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        const float rgb[3] = {0.8f, 0.3f, 0.2f};
        REQUIRE(clay_item_set_color(it, rgb) == CLAY_OK);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, layer, it, &id) == CLAY_OK);
        clay_item_destroy(it);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// A tetrahedron, so a mesh round trip has something with a known shape.
const float kPositions[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
const std::uint32_t kIndices[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};

void tetra(MeshHandle* out) {
    REQUIRE(clay_mesh_from_triangles(kPositions, 4, kIndices, 12, &out->m) == CLAY_OK);
}

float eval_at(const clay_document* d, float x, float y, float z) {
    const float p[3] = {x, y, z};
    float out = 0.0f;
    REQUIRE(clay_eval_points(d, nullptr, p, 1, &out, nullptr) == CLAY_OK);
    return out;
}

}  // namespace

TEST_CASE("c abi: a document round-trips through memory") {
    Doc doc;
    Blob blob;
    REQUIRE(clay_document_save_memory(doc.d, &blob.b) == CLAY_OK);
    CHECK(clay_blob_size(blob.b) > 0);
    CHECK(clay_blob_data(blob.b) != nullptr);

    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(blob.b), clay_blob_size(blob.b), &back) ==
            CLAY_OK);
    REQUIRE(back != nullptr);

    for (float t = -1.0f; t <= 1.0f; t += 0.25f)
        CHECK(eval_at(back, t, 0.1f, -0.2f) == doctest::Approx(eval_at(doc.d, t, 0.1f, -0.2f)));

    size_t layers = 0;
    REQUIRE(clay_document_layer_count(back, &layers) == CLAY_OK);
    CHECK(layers == 1);
    clay_document_destroy(back);
}

TEST_CASE("c abi: memory and file agree byte for byte") {
    // The whole argument for this change: one serializer, reached two ways.
    Doc doc;
    const std::string path = temp_path("clay_memory_io.clayspace");
    REQUIRE(clay_document_save(doc.d, path.c_str()) == CLAY_OK);

    Blob blob;
    REQUIRE(clay_document_save_memory(doc.d, &blob.b) == CLAY_OK);
    CHECK(blob.bytes() == read_file(path));
    std::filesystem::remove(path);
}

TEST_CASE("c abi: every mesh format round-trips through memory") {
    MeshHandle mesh;
    tetra(&mesh);

    for (const char* format : {"obj", "ply", "fbx", "glb"}) {
        CAPTURE(format);
        Blob blob;
        REQUIRE(clay_mesh_save_memory(mesh.m, format, &blob.b) == CLAY_OK);
        CHECK(clay_blob_size(blob.b) > 0);

        MeshHandle back;
        REQUIRE(clay_mesh_load_memory(clay_blob_data(blob.b), clay_blob_size(blob.b), format,
                                      nullptr, &back.m) == CLAY_OK);
        CHECK(clay_mesh_index_count(back.m) == 12);
        CHECK(clay_mesh_vertex_count(back.m) > 0);

        // The writers agree with their file counterparts. OBJ is the one
        // exception and it is stated: see the mtllib case below.
        if (std::strcmp(format, "obj") != 0) {
            const std::string path = temp_path("clay_memory_io_mesh") + "." + format;
            REQUIRE(clay_mesh_save(mesh.m, path.c_str()) == CLAY_OK);
            CHECK(blob.bytes() == read_file(path));
            std::filesystem::remove(path);
        }
    }
}

TEST_CASE("c abi: an in-memory OBJ names no material file") {
    // The path form writes a companion .mtl and names it in a mtllib line. A
    // buffer has no companion, so naming one would dangle.
    MeshHandle mesh;
    tetra(&mesh);
    Blob blob;
    REQUIRE(clay_mesh_save_memory(mesh.m, "obj", &blob.b) == CLAY_OK);
    const std::vector<std::uint8_t> bytes = blob.bytes();
    const std::string text(bytes.begin(), bytes.end());
    CHECK(text.find("mtllib") == std::string::npos);
    CHECK(text.find("v ") != std::string::npos);  // it is still an OBJ
}

TEST_CASE("c abi: the format name is matched case-insensitively both ways") {
    MeshHandle mesh;
    tetra(&mesh);
    Blob upper;
    REQUIRE(clay_mesh_save_memory(mesh.m, "PLY", &upper.b) == CLAY_OK);
    Blob lower;
    REQUIRE(clay_mesh_save_memory(mesh.m, "ply", &lower.b) == CLAY_OK);
    CHECK(upper.bytes() == lower.bytes());

    MeshHandle back;
    CHECK(clay_mesh_load_memory(clay_blob_data(upper.b), clay_blob_size(upper.b), "Ply", nullptr,
                                &back.m) == CLAY_OK);
}

TEST_CASE("c abi: the mesh WRITER matches an extension case-insensitively too") {
    // Regression. clay_mesh_load lowercased the extension and clay_mesh_save
    // did not, so a host could load MODEL.OBJ and then be refused when it
    // saved back to the path it had just read from.
    MeshHandle mesh;
    tetra(&mesh);
    const std::string path = temp_path("CLAY_MEMORY_IO.PLY");
    CHECK(clay_mesh_save(mesh.m, path.c_str()) == CLAY_OK);
    MeshHandle back;
    CHECK(clay_mesh_load(path.c_str(), nullptr, &back.m) == CLAY_OK);
    std::filesystem::remove(path);
}

TEST_CASE("c abi: the borrowed bytes survive an edit to what produced them") {
    Doc doc;
    Blob blob;
    REQUIRE(clay_document_save_memory(doc.d, &blob.b) == CLAY_OK);
    const std::vector<std::uint8_t> before = blob.bytes();

    const float box[3] = {0.25f, 0.25f, 0.25f};
    clay_item* it = clay_item_create(CLAY_PRIM_BOX, box, 3);
    REQUIRE(it != nullptr);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);

    CHECK(blob.bytes() == before);  // serialized when the handle was made
}

TEST_CASE("c abi: memory io refuses what it should") {
    Doc doc;
    MeshHandle mesh;
    tetra(&mesh);

    clay_document* out_doc = nullptr;
    CHECK(clay_document_load_memory(nullptr, 10, &out_doc) == CLAY_ERROR_INVALID_ARGUMENT);
    const std::uint8_t byte = 0;
    CHECK(clay_document_load_memory(&byte, 0, &out_doc) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_load_memory(&byte, 1, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_save_memory(nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(out_doc == nullptr);

    // Not a .clayspace at all: refused, and nothing produced to free.
    const std::uint8_t junk[16] = {'n', 'o', 't', ' ', 'a', ' ', 'd', 'o',
                                   'c', 0,   1,   2,   3,   4,   5,   6};
    CHECK(clay_document_load_memory(junk, sizeof(junk), &out_doc) != CLAY_OK);
    CHECK(out_doc == nullptr);

    // An unknown format is refused rather than served as some default.
    Blob blob;
    CHECK(clay_mesh_save_memory(mesh.m, "stl", &blob.b) == CLAY_ERROR_UNSUPPORTED);
    CHECK(blob.b == nullptr);
    CHECK(clay_mesh_save_memory(mesh.m, nullptr, &blob.b) == CLAY_ERROR_INVALID_ARGUMENT);

    MeshHandle back;
    CHECK(clay_mesh_load_memory(junk, sizeof(junk), "stl", nullptr, &back.m) ==
          CLAY_ERROR_UNSUPPORTED);
    CHECK(clay_mesh_load_memory(nullptr, 4, "ply", nullptr, &back.m) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(back.m == nullptr);
}

TEST_CASE("c abi: a truncated buffer stays in bounds") {
    MeshHandle mesh;
    tetra(&mesh);
    Blob blob;
    REQUIRE(clay_mesh_save_memory(mesh.m, "ply", &blob.b) == CLAY_OK);
    const std::vector<std::uint8_t> full = blob.bytes();
    REQUIRE(full.size() > 8);

    // Every prefix is refused or loads something coherent; none may read past
    // the length it was given. Under ASan this is the case that would catch it.
    for (std::size_t cut = 1; cut < full.size(); cut += 7) {
        MeshHandle back;
        const clay_result r =
            clay_mesh_load_memory(full.data(), cut, "ply", nullptr, &back.m);
        if (r == CLAY_OK) CHECK(back.m != nullptr);
    }
}

TEST_CASE("c abi: the import budget guards a buffer as it guards a file") {
    MeshHandle mesh;
    tetra(&mesh);
    Blob blob;
    REQUIRE(clay_mesh_save_memory(mesh.m, "ply", &blob.b) == CLAY_OK);

    clay_import_budget budget{};
    budget.struct_size = sizeof(budget);
    budget.max_vertices = 2;  // the tetrahedron has four
    budget.max_triangles = 0;

    MeshHandle back;
    CHECK(clay_mesh_load_memory(clay_blob_data(blob.b), clay_blob_size(blob.b), "ply", &budget,
                                &back.m) == CLAY_ERROR_BUDGET_EXCEEDED);
    CHECK(back.m == nullptr);

    // And a budget that fits lets it through.
    budget.max_vertices = 1000;
    MeshHandle ok;
    CHECK(clay_mesh_load_memory(clay_blob_data(blob.b), clay_blob_size(blob.b), "ply", &budget,
                                &ok.m) == CLAY_OK);
}

TEST_CASE("c abi: a zero-length blob is legal and reports itself") {
    // clay_blob_data may return NULL only for an empty blob, and the
    // accessors answer for a null handle rather than dereferencing it.
    CHECK(clay_blob_data(nullptr) == nullptr);
    CHECK(clay_blob_size(nullptr) == 0);
    clay_blob_destroy(nullptr);  // must not crash
}
