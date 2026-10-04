#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

// `clay_mesh_from_arrays` (issue #661): a host hands the library a mesh that
// carries the per-vertex UVs, normals and colours it computed itself.
//
// Before this, `clay_mesh_from_triangles` and `clay_mesh_from_quads` copied
// positions and indices and nothing else, and the only entry point that
// attached UVs was the OBJ reader — so a host with a retopology laid out by its
// own unwrapper wrote `v`/`vn`/`vt` text into memory and read it back through
// `clay_mesh_load_memory(..., "obj", ...)` to get a mesh layer that kept them.
//
// The attribute floats are compared BIT-EXACTLY (memcmp), not with a
// tolerance: a constructor copies, and the only correct answer is the input.

namespace {

struct Mesh {
    clay_mesh* m = nullptr;
    ~Mesh() {
        if (m) clay_mesh_destroy(m);
    }
    Mesh() = default;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
};

// A unit square in z=0, as one quad (a,b,c,d) or two triangles.
const std::vector<float> kPositions = {
    0.0f, 0.0f, 0.0f,  // a
    1.0f, 0.0f, 0.0f,  // b
    1.0f, 1.0f, 0.0f,  // c
    0.0f, 1.0f, 0.0f,  // d
};
// Values chosen so a dropped, reordered or re-normalised channel cannot pass:
// none is a unit vector, none repeats across vertices, and the uvs are not the
// positions' x/y.
const std::vector<float> kNormals = {
    0.1f, 0.2f, 0.9f, -0.3f, 0.25f, 0.875f, 0.0625f, -0.5f, 0.75f, 0.33f, 0.44f, 0.55f,
};
const std::vector<float> kColors = {
    0.9f, 0.1f, 0.2f, 0.3f, 0.8f, 0.4f, 0.125f, 0.375f, 0.7f, 1.0f, 0.5f, 0.0f,
};
const std::vector<float> kUvs = {
    0.0625f, 0.125f, 0.9375f, 0.1f, 0.875f, 0.8125f, 0.2f, 0.7f,
};
const std::vector<std::uint32_t> kTriangles = {0, 1, 2, 0, 2, 3};
const std::vector<std::uint32_t> kQuad = {0, 1, 2, 3};

clay_mesh_arrays full_triangle_arrays() {
    clay_mesh_arrays in;
    std::memset(&in, 0, sizeof in);
    in.struct_size = sizeof in;
    in.positions = kPositions.data();
    in.vertex_count = 4;
    in.normals = kNormals.data();
    in.colors = kColors.data();
    in.uvs = kUvs.data();
    in.indices = kTriangles.data();
    in.index_count = kTriangles.size();
    return in;
}

bool same_bits(const float* got, const std::vector<float>& want) {
    return got != nullptr && std::memcmp(got, want.data(), want.size() * sizeof(float)) == 0;
}

void check_attributes(const clay_mesh* mesh) {
    REQUIRE(mesh != nullptr);
    REQUIRE(clay_mesh_vertex_count(mesh) == 4);
    CHECK(same_bits(clay_mesh_positions(mesh), kPositions));
    CHECK(same_bits(clay_mesh_normals(mesh), kNormals));
    CHECK(same_bits(clay_mesh_colors(mesh), kColors));
    CHECK(same_bits(clay_mesh_uvs(mesh), kUvs));
}

clay_result build(const clay_mesh_arrays& in, Mesh* out) {
    return clay_mesh_from_arrays(&in, &out->m);
}

}  // namespace

TEST_CASE("a mesh built from arrays gives its uvs, normals and colours back bit-exactly") {
    Mesh mesh;
    REQUIRE(build(full_triangle_arrays(), &mesh) == CLAY_OK);
    check_attributes(mesh.m);
    REQUIRE(clay_mesh_index_count(mesh.m) == 6);
    const std::uint32_t* tris = clay_mesh_indices(mesh.m);
    REQUIRE(tris != nullptr);
    for (int i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK(tris[i] == kTriangles[static_cast<std::size_t>(i)]);
    }
    // Triangles are taken as given; they carry no quads.
    CHECK(clay_mesh_quad_count(mesh.m) == 0);
}

TEST_CASE("NULL attributes leave the mesh without them") {
    clay_mesh_arrays in = full_triangle_arrays();
    in.normals = nullptr;
    in.colors = nullptr;
    in.uvs = nullptr;
    Mesh mesh;
    REQUIRE(build(in, &mesh) == CLAY_OK);
    CHECK(clay_mesh_normals(mesh.m) == nullptr);
    CHECK(clay_mesh_colors(mesh.m) == nullptr);
    CHECK(clay_mesh_uvs(mesh.m) == nullptr);

    // Each attribute is independent: uvs alone is the retopology case.
    in.uvs = kUvs.data();
    Mesh uv_only;
    REQUIRE(build(in, &uv_only) == CLAY_OK);
    CHECK(clay_mesh_normals(uv_only.m) == nullptr);
    CHECK(clay_mesh_colors(uv_only.m) == nullptr);
    CHECK(same_bits(clay_mesh_uvs(uv_only.m), kUvs));
}

TEST_CASE("quads given to from_arrays derive the triangles from_quads derives") {
    clay_mesh_arrays in = full_triangle_arrays();
    in.indices = nullptr;
    in.index_count = 0;
    in.quad_indices = kQuad.data();
    in.quad_index_count = kQuad.size();
    Mesh mesh;
    REQUIRE(build(in, &mesh) == CLAY_OK);
    check_attributes(mesh.m);
    CHECK(clay_mesh_quad_count(mesh.m) == 1);

    Mesh reference;
    REQUIRE(clay_mesh_from_quads(kPositions.data(), 4, kQuad.data(), kQuad.size(),
                                 &reference.m) == CLAY_OK);
    REQUIRE(clay_mesh_index_count(mesh.m) == clay_mesh_index_count(reference.m));
    CHECK(std::memcmp(clay_mesh_indices(mesh.m), clay_mesh_indices(reference.m),
                      6 * sizeof(std::uint32_t)) == 0);
    CHECK(std::memcmp(clay_mesh_quads(mesh.m), kQuad.data(), 4 * sizeof(std::uint32_t)) == 0);
}

TEST_CASE("attributes survive a mesh-layer attach, undo/redo, and a save and load") {
    const char* path = "test_c_mesh_from_arrays_roundtrip.clayspace";
    {
        Mesh built;
        REQUIRE(build(full_triangle_arrays(), &built) == CLAY_OK);

        clay_document* doc = clay_document_create();
        REQUIRE(doc != nullptr);
        REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
        clay_mesh_layer_desc desc;
        std::memset(&desc, 0, sizeof desc);
        desc.struct_size = sizeof desc;
        desc.name = "retopo";
        clay_layer_id layer = 0;
        clay_mesh* stored = nullptr;
        REQUIRE(clay_document_add_mesh_layer(doc, built.m, &desc, &layer, &stored) == CLAY_OK);
        check_attributes(stored);

        // Undoing the creation keeps the payload, so the redo brings it back.
        int32_t moved = 0;
        REQUIRE(clay_document_undo(doc, &moved) == CLAY_OK);
        CHECK(moved == 1);
        REQUIRE(clay_document_redo(doc, &moved) == CLAY_OK);
        CHECK(moved == 1);
        clay_mesh* redone = nullptr;
        REQUIRE(clay_document_mesh_layer_by_id(doc, layer, &redone) == CLAY_OK);
        check_attributes(redone);

        REQUIRE(clay_document_save(doc, path) == CLAY_OK);
        clay_document_destroy(doc);
    }

    clay_document* loaded = nullptr;
    REQUIRE(clay_document_load(path, &loaded) == CLAY_OK);
    REQUIRE(loaded != nullptr);
    clay_mesh* back = nullptr;
    clay_layer_id back_layer = 0;
    REQUIRE(clay_document_mesh_layer(loaded, "retopo", &back_layer, &back) == CLAY_OK);
    check_attributes(back);
    clay_document_destroy(loaded);
    std::remove(path);
}

TEST_CASE("clay_mesh_from_arrays refuses a malformed call and leaves out_mesh NULL") {
    auto refuses = [](const clay_mesh_arrays& in) {
        clay_mesh* out = reinterpret_cast<clay_mesh*>(0x1);  // must be cleared
        const clay_result r = clay_mesh_from_arrays(&in, &out);
        CHECK(out == nullptr);
        if (out) clay_mesh_destroy(out);
        return r;
    };

    SUBCASE("null descriptor or out pointer") {
        clay_mesh* out = nullptr;
        CHECK(clay_mesh_from_arrays(nullptr, &out) == CLAY_ERROR_INVALID_ARGUMENT);
        const clay_mesh_arrays in = full_triangle_arrays();
        CHECK(clay_mesh_from_arrays(&in, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("null positions") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.positions = nullptr;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("zero vertices") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.vertex_count = 0;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("an index count that is not whole triangles") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.index_count = 5;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a quad index count that is not whole quads") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.indices = nullptr;
        in.index_count = 0;
        in.quad_indices = kQuad.data();
        in.quad_index_count = 3;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("an index past the vertices") {
        const std::vector<std::uint32_t> bad = {0, 1, 4};
        clay_mesh_arrays in = full_triangle_arrays();
        in.indices = bad.data();
        in.index_count = bad.size();
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a quad corner past the vertices") {
        const std::vector<std::uint32_t> bad = {0, 1, 2, 4};
        clay_mesh_arrays in = full_triangle_arrays();
        in.indices = nullptr;
        in.index_count = 0;
        in.quad_indices = bad.data();
        in.quad_index_count = bad.size();
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("both index kinds supplied") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.quad_indices = kQuad.data();
        in.quad_index_count = kQuad.size();
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("neither index kind supplied") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.indices = nullptr;
        in.index_count = 0;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a count without its pointer") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.indices = nullptr;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("an undersized struct_size") {
        clay_mesh_arrays in = full_triangle_arrays();
        in.struct_size = 4;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
        in.struct_size = 0;
        CHECK(refuses(in) == CLAY_ERROR_INVALID_ARGUMENT);
    }
}
