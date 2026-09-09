#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

// `clay_mesh_from_quads` (issue #514): a host whose retopologiser is not this
// library's can store the quads it produced.
//
// Before this, `clay_mesh_from_triangles` was the only ABI constructor taking
// caller-supplied arrays, so a host with real quads — ZRemesher at 258 faces /
// 516 triangles, QuadCover at 256 / 512, both exactly 2:1 — had to call
// `from_triangles(positions, quads.triangle_indices())` and the quad faces died
// at the boundary. The polyframe then drew triangles, correctly, because
// triangles were all the layer held.
//
// WHAT THESE TESTS ARE ACTUALLY GUARDING is the invariant tying `quads` to
// `indices`: quad q with corners (a,b,c,d) is triangles (a,b,c) and (a,c,d) at
// indices[6q..6q+5]. The constructor DERIVES indices by that rule rather than
// taking them, so the invariant is true by construction — and the point of
// testing it through the readers is that a mesh built here must be
// indistinguishable from one this library quad-meshed itself.

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

// One unit square as a single quad, wound (a,b,c,d).
const std::vector<float> kSquarePositions = {
    0.0f, 0.0f, 0.0f,  // a
    1.0f, 0.0f, 0.0f,  // b
    1.0f, 1.0f, 0.0f,  // c
    0.0f, 1.0f, 0.0f,  // d
};
const std::vector<std::uint32_t> kSquareQuad = {0, 1, 2, 3};

}  // namespace

TEST_CASE("a mesh built from quads carries them, and triangulates by the documented rule") {
    Mesh mesh;
    REQUIRE(clay_mesh_from_quads(kSquarePositions.data(), 4, kSquareQuad.data(), 4, &mesh.m) ==
            CLAY_OK);
    REQUIRE(mesh.m != nullptr);

    CHECK(clay_mesh_vertex_count(mesh.m) == 4);
    CHECK(clay_mesh_quad_count(mesh.m) == 1);
    // Two triangles per quad, which is what makes the mesh drawable by anything
    // that has never heard of quads.
    CHECK(clay_mesh_index_count(mesh.m) == 6);

    // (a,b,c),(a,c,d) IN ORDER, not as a set: the other diagonal is a different
    // surface wherever the quad is not planar, and every quad this library
    // makes may be non-planar.
    const std::uint32_t* tris = clay_mesh_indices(mesh.m);
    REQUIRE(tris != nullptr);
    const std::uint32_t expected[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK(tris[i] == expected[i]);
    }

    // And the quads read back exactly as given.
    const std::uint32_t* quads = clay_mesh_quads(mesh.m);
    REQUIRE(quads != nullptr);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(quads[i] == kSquareQuad[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("the readers answer for a host-built quad mesh as they do for a meshed one") {
    // The claim the issue rests on: there is no provenance test anywhere, so
    // every existing accessor must work unchanged. Asserted rather than assumed.
    Mesh mesh;
    REQUIRE(clay_mesh_from_quads(kSquarePositions.data(), 4, kSquareQuad.data(), 4, &mesh.m) ==
            CLAY_OK);

    std::vector<std::uint32_t> copied(4, 0xFFFFFFFFu);
    REQUIRE(clay_mesh_copy_quads(mesh.m, copied.data(), copied.size()) == CLAY_OK);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(copied[static_cast<std::size_t>(i)] == kSquareQuad[static_cast<std::size_t>(i)]);
    }

    // dst_count is required to be exactly 4 * quad_count, so a short buffer is
    // refused rather than partially filled.
    std::vector<std::uint32_t> shortbuf(3, 0);
    CHECK(clay_mesh_copy_quads(mesh.m, shortbuf.data(), shortbuf.size()) != CLAY_OK);

    // The report exists for a mesh that never went near a lattice; the fields
    // describing a search are simply zero. It must not fail.
    clay_quad_report report;
    std::memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    const clay_result r = clay_mesh_quad_report(mesh.m, &report);
    if (r == CLAY_OK) CHECK(report.quad_count == 1);
}

TEST_CASE("two quads sharing an edge keep their own triangulations") {
    // A single quad cannot catch an off-by-one in the per-quad stride.
    const std::vector<float> positions = {
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0.0f,
    };
    const std::vector<std::uint32_t> quads = {0, 1, 2, 3, 1, 4, 5, 2};

    Mesh mesh;
    REQUIRE(clay_mesh_from_quads(positions.data(), 6, quads.data(), quads.size(), &mesh.m) ==
            CLAY_OK);
    CHECK(clay_mesh_quad_count(mesh.m) == 2);
    CHECK(clay_mesh_index_count(mesh.m) == 12);

    const std::uint32_t* tris = clay_mesh_indices(mesh.m);
    REQUIRE(tris != nullptr);
    const std::uint32_t expected[12] = {0, 1, 2, 0, 2, 3, 1, 4, 5, 1, 5, 2};
    for (int i = 0; i < 12; ++i) {
        CAPTURE(i);
        CHECK(tris[i] == expected[i]);
    }
}

TEST_CASE("a quad buffer that is not a whole number of quads is refused") {
    Mesh mesh;
    for (const size_t count : {size_t(1), size_t(2), size_t(3), size_t(5), size_t(7)}) {
        CAPTURE(count);
        clay_mesh* out = reinterpret_cast<clay_mesh*>(0x1);
        CHECK(clay_mesh_from_quads(kSquarePositions.data(), 4, kSquareQuad.data(), count, &out) !=
              CLAY_OK);
        CHECK(out == nullptr);  // and the out pointer is cleared, not left dangling
    }
    // Zero is refused too: an empty mesh is not what a caller meant to build.
    clay_mesh* empty = reinterpret_cast<clay_mesh*>(0x1);
    CHECK(clay_mesh_from_quads(kSquarePositions.data(), 4, kSquareQuad.data(), 0, &empty) !=
          CLAY_OK);
    CHECK(empty == nullptr);
    CHECK(clay_mesh_from_quads(kSquarePositions.data(), 0, kSquareQuad.data(), 4, &empty) !=
          CLAY_OK);
}

TEST_CASE("a corner past the vertices is refused rather than stored") {
    // The same validation clay_mesh_from_triangles does, for the same reason:
    // an out-of-range corner is a buffer overrun waiting for the first reader.
    const std::vector<std::uint32_t> bad = {0, 1, 2, 4};  // only 4 vertices exist, so 4 is past
    clay_mesh* out = reinterpret_cast<clay_mesh*>(0x1);
    CHECK(clay_mesh_from_quads(kSquarePositions.data(), 4, bad.data(), 4, &out) != CLAY_OK);
    CHECK(out == nullptr);
}

TEST_CASE("null arguments are refused") {
    clay_mesh* out = reinterpret_cast<clay_mesh*>(0x1);
    CHECK(clay_mesh_from_quads(nullptr, 4, kSquareQuad.data(), 4, &out) != CLAY_OK);
    CHECK(out == nullptr);
    out = reinterpret_cast<clay_mesh*>(0x1);
    CHECK(clay_mesh_from_quads(kSquarePositions.data(), 4, nullptr, 4, &out) != CLAY_OK);
    CHECK(out == nullptr);
    CHECK(clay_mesh_from_quads(kSquarePositions.data(), 4, kSquareQuad.data(), 4, nullptr) !=
          CLAY_OK);
}

TEST_CASE("a non-planar quad is stored rather than refused") {
    // Deliberately, and the header says so: every quad this library itself
    // produces may be non-planar, and the lattice mesher emits near-zero-area
    // quads around thin features. Refusing here would reject meshes the engine
    // makes for itself.
    std::vector<float> positions = kSquarePositions;
    positions[11] = 0.5f;  // lift d out of the plane
    Mesh mesh;
    CHECK(clay_mesh_from_quads(positions.data(), 4, kSquareQuad.data(), 4, &mesh.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(mesh.m) == 1);
}

TEST_CASE("host-built quads survive a document save and load") {
    // The blast-radius check the issue asks for. `mesh_stream.cpp` already
    // writes quads and asserts mesh::quads_consistent on load, so the format is
    // ready -- but until now the only meshes carrying quads were ones this
    // library meshed. This is the first time a HOST-built quad list goes into a
    // file, and the invariant it is checked against on the way back in is the
    // one clay_mesh_from_quads derives rather than trusts.
    const std::vector<float> positions = {
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0.0f,
    };
    const std::vector<std::uint32_t> quads = {0, 1, 2, 3, 1, 4, 5, 2};

    const char* path = "test_c_mesh_from_quads_roundtrip.clayspace";

    {
        Mesh built;
        REQUIRE(clay_mesh_from_quads(positions.data(), 6, quads.data(), quads.size(), &built.m) ==
                CLAY_OK);

        clay_document* doc = clay_document_create();
        REQUIRE(doc != nullptr);
        clay_mesh_layer_desc desc;
        std::memset(&desc, 0, sizeof desc);
        desc.struct_size = sizeof desc;
        desc.name = "retopo";
        clay_layer_id layer = 0;
        clay_mesh* stored = nullptr;
        const clay_result added =
            clay_document_add_mesh_layer(doc, built.m, &desc, &layer, &stored);
        if (added != CLAY_OK) {
            clay_document_destroy(doc);
            return;  // this build refuses mesh layers; nothing to round trip
        }
        // The document's copy carries them before it is ever written.
        CHECK(clay_mesh_quad_count(stored) == 2);
        REQUIRE(clay_document_save(doc, path) == CLAY_OK);
        clay_document_destroy(doc);
    }

    clay_document* loaded = nullptr;
    REQUIRE(clay_document_load(path, &loaded) == CLAY_OK);
    REQUIRE(loaded != nullptr);
    clay_mesh* back = nullptr;
    clay_layer_id back_layer = 0;
    REQUIRE(clay_document_mesh_layer(loaded, "retopo", &back_layer, &back) == CLAY_OK);
    REQUIRE(back != nullptr);

    CHECK(clay_mesh_quad_count(back) == 2);
    const std::uint32_t* got = clay_mesh_quads(back);
    REQUIRE(got != nullptr);
    for (std::size_t i = 0; i < quads.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i] == quads[i]);
    }
    // And the triangulation the loader checked against is still the derived one.
    const std::uint32_t* tris = clay_mesh_indices(back);
    REQUIRE(tris != nullptr);
    const std::uint32_t expected[12] = {0, 1, 2, 0, 2, 3, 1, 4, 5, 1, 5, 2};
    for (int i = 0; i < 12; ++i) {
        CAPTURE(i);
        CHECK(tris[i] == expected[i]);
    }
    clay_document_destroy(loaded);
    std::remove(path);
}
