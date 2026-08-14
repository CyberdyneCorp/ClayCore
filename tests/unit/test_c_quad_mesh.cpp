#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "clay.h"

// Quad meshing across the C ABI (c-abi spec: quad meshing across the ABI).
//
// The interesting cases are the refusals and the report. A host that asks for
// fifty thousand quads and receives thirty-one thousand has to be able to find
// out that a limit stopped the search rather than that something broke, and a
// host that asks a document for the voxel-only faces mode has to be told so
// rather than handed a smooth mesh that looks plausible.

namespace {

clay_quad_params quad_defaults() {
    clay_quad_params p{};
    p.struct_size = sizeof(clay_quad_params);
    return p;
}

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
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, layer, it, &id) == CLAY_OK);
        clay_item_destroy(it);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    operator clay_document*() const { return d; }
};

struct Owned {
    clay_mesh* m = nullptr;
    ~Owned() { clay_mesh_destroy(m); }
    Owned() = default;
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    operator clay_mesh*() const { return m; }
};

struct CGrid {
    clay_voxel_grid* g = nullptr;
    explicit CGrid(float voxel_size = 0.1f, int extent = 7)
        : g(clay_voxel_grid_create(voxel_size)) {
        REQUIRE(g != nullptr);
        int32_t index = 0;
        const float rgb[3] = {0.8f, 0.4f, 0.2f};
        REQUIRE(clay_voxel_palette_add(g, rgb, &index) == CLAY_OK);
        const int32_t a[3] = {0, 0, 0}, b[3] = {extent, extent, extent};
        REQUIRE(clay_voxel_fill_box(g, a, b, index) == CLAY_OK);
    }
    ~CGrid() { clay_voxel_grid_destroy(g); }
    operator clay_voxel_grid*() const { return g; }
};

// The invariant mesh/mesh_data.h states, asserted through the ABI's own
// accessors rather than through the C++ type — a host sees only these.
void check_invariant(const clay_mesh* m) {
    const size_t quads = clay_mesh_quad_count(m);
    REQUIRE(clay_mesh_index_count(m) == quads * 6);
    const uint32_t* q = clay_mesh_quads(m);
    const uint32_t* t = clay_mesh_indices(m);
    REQUIRE(q != nullptr);
    REQUIRE(t != nullptr);
    const size_t vertices = clay_mesh_vertex_count(m);
    for (size_t i = 0; i < quads; ++i) {
        for (int c = 0; c < 4; ++c) REQUIRE(q[i * 4 + c] < vertices);
        CHECK(t[i * 6 + 0] == q[i * 4 + 0]);
        CHECK(t[i * 6 + 1] == q[i * 4 + 1]);
        CHECK(t[i * 6 + 2] == q[i * 4 + 2]);
        CHECK(t[i * 6 + 3] == q[i * 4 + 0]);
        CHECK(t[i * 6 + 4] == q[i * 4 + 2]);
        CHECK(t[i * 6 + 5] == q[i * 4 + 3]);
    }
}

}  // namespace

TEST_CASE("c abi: a document quad-meshes and the triangles are the quads") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.cell_size = 0.06f;
    Owned mesh;
    REQUIRE(clay_document_mesh_quads(doc, &p, &mesh.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(mesh) > 0);
    check_invariant(mesh);

    // The copy-out takes the exact element count, as the index copy does, and
    // refuses anything else rather than truncating into a short buffer.
    const size_t elements = clay_mesh_quad_count(mesh) * 4;
    std::vector<uint32_t> dst(elements);
    CHECK(clay_mesh_copy_quads(mesh, dst.data(), elements) == CLAY_OK);
    CHECK(dst[0] == clay_mesh_quads(mesh)[0]);
    CHECK(clay_mesh_copy_quads(mesh, dst.data(), elements - 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_copy_quads(mesh, dst.data(), elements + 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_copy_quads(mesh, nullptr, elements) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c abi: the meshers that predate quads still report none") {
    Doc doc;
    clay_mesh_params p{};
    p.struct_size = sizeof(clay_mesh_params);
    p.resolution = 32;
    Owned mesh;
    REQUIRE(clay_document_mesh(doc, &p, &mesh.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(mesh) == 0);
    CHECK(clay_mesh_quads(mesh) == nullptr);
    CHECK(clay_mesh_index_count(mesh) > 0);

    // And it has no report to give, rather than a report of zeroes a host
    // cannot tell from a search that found nothing.
    clay_quad_report report{};
    report.struct_size = sizeof(clay_quad_report);
    CHECK(clay_mesh_quad_report(mesh, &report) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::string(clay_last_error()).find("not produced by a quad mesher") !=
          std::string::npos);
}

TEST_CASE("c abi: faces mode on a document is refused, not substituted") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.cell_size = 0.1f;
    p.mode = CLAY_QUAD_FACES;
    clay_mesh* mesh = nullptr;
    CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(mesh == nullptr);
    CHECK(std::string(clay_last_error()).find("voxel mode") != std::string::npos);
}

TEST_CASE("c abi: an unknown quad mode is rejected rather than defaulted") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.cell_size = 0.1f;
    p.mode = 7;
    clay_mesh* mesh = nullptr;
    CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(mesh == nullptr);
}

TEST_CASE("c abi: a descriptor that predates the count fields still meshes") {
    // The struct_size contract, exercised with the layout this descriptor
    // would have had before a target could be asked for: the appended fields
    // take their zero defaults, which means the dual mode and no search.
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.struct_size = offsetof(clay_quad_params, level) + sizeof(uint32_t);
    p.cell_size = 0.08f;
    p.target_quads = 999999;  // past the declared size: must be ignored
    Owned mesh;
    REQUIRE(clay_document_mesh_quads(doc, &p, &mesh.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(mesh) > 0);

    clay_quad_report report{};
    report.struct_size = sizeof(clay_quad_report);
    REQUIRE(clay_mesh_quad_report(mesh, &report) == CLAY_OK);
    CHECK(report.target_quads == 0);
    CHECK(report.iterations == 0);
    CHECK(report.cell_size == doctest::Approx(0.08f));
}

TEST_CASE("c abi: a struct_size below the original layout is refused") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.struct_size = 8;
    p.cell_size = 0.1f;
    clay_mesh* mesh = nullptr;
    CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(mesh == nullptr);
}

TEST_CASE("c abi: neither a cell size nor a target names no lattice") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    clay_mesh* mesh = nullptr;
    CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(mesh == nullptr);
}

TEST_CASE("c abi: the report explains the number the host actually got") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.target_quads = 3000;
    Owned mesh;
    REQUIRE(clay_document_mesh_quads(doc, &p, &mesh.m) == CLAY_OK);

    clay_quad_report report{};
    report.struct_size = sizeof(clay_quad_report);
    REQUIRE(clay_mesh_quad_report(mesh, &report) == CLAY_OK);
    CHECK(report.quad_count == clay_mesh_quad_count(mesh));
    CHECK(report.target_quads == 3000);
    CHECK(report.cell_size > 0.0f);
    CHECK(report.iterations > 0);
    CHECK(report.within_tolerance == 1);
    CHECK(report.clamped == 0);
    check_invariant(mesh);
}

TEST_CASE("c abi: an iteration cap nobody could have meant is refused") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.target_quads = 3000;
    p.max_iterations = 100000;  // a byte count where a mesh count belongs
    clay_mesh* mesh = nullptr;
    CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(mesh == nullptr);
}

// REGRESSION (review round 2): the two bindings disagreed about what the count
// knobs' zero MEANS. clay_quad_params documents "<= 0 means the default" and
// this binding honoured it, while pyclay raised on tolerance 0 and on
// max_iterations 0 — the documented C defaults were errors in Python. The rule
// is one rule now: 0 defaults in both, a tolerance of 1 or more says nothing
// and is refused in both, and a target past CLAY_MAX_BATCH is refused in both.
TEST_CASE("c abi: the count knobs default at zero and are bounded at the far end") {
    Doc doc;
    SUBCASE("zero is the default and meshes, it does not fail") {
        clay_quad_params p = quad_defaults();
        p.target_quads = 3000;
        p.tolerance = 0.0f;      // means 0.10
        p.max_iterations = 0;    // means 4
        Owned mesh;
        REQUIRE(clay_document_mesh_quads(doc, &p, &mesh.m) == CLAY_OK);
        clay_quad_report report{};
        report.struct_size = sizeof(report);
        REQUIRE(clay_mesh_quad_report(mesh, &report) == CLAY_OK);
        CHECK(report.iterations > 0);
        CHECK(report.quad_count > 0);
    }
    SUBCASE("a tolerance of 100% of the target reports nothing and is refused") {
        clay_quad_params p = quad_defaults();
        p.target_quads = 3000;
        p.tolerance = 1.0f;
        clay_mesh* mesh = nullptr;
        CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(mesh == nullptr);
    }
    SUBCASE("a negative iteration cap is a mistake, not a request for the default") {
        clay_quad_params p = quad_defaults();
        p.target_quads = 3000;
        p.max_iterations = -1;
        clay_mesh* mesh = nullptr;
        CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(mesh == nullptr);
    }
    SUBCASE("a target past the batch ceiling is refused before anything is meshed") {
        clay_quad_params p = quad_defaults();
        p.target_quads = static_cast<std::uint64_t>(CLAY_MAX_BATCH) + 1;
        clay_mesh* mesh = nullptr;
        CHECK(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(mesh == nullptr);
    }
}

TEST_CASE("c abi: a voxel grid quad-meshes in both modes") {
    CGrid grid(0.1f, 7);
    clay_quad_params p = quad_defaults();
    Owned dual;
    REQUIRE(clay_voxel_mesh_quads(grid, &p, &dual.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(dual) > 0);
    check_invariant(dual);

    p.mode = CLAY_QUAD_FACES;
    Owned faces;
    REQUIRE(clay_voxel_mesh_quads(grid, &p, &faces.m) == CLAY_OK);
    // One quad per exposed face of an 8^3 block: six faces of 64.
    CHECK(clay_mesh_quad_count(faces) == 6 * 8 * 8);
    check_invariant(faces);
    // No vertex normals: a welded corner faces three ways at once.
    CHECK(clay_mesh_normals(faces) == nullptr);

    // And the meshers that predate this are untouched.
    Owned greedy;
    REQUIRE(clay_voxel_mesh(grid, &greedy.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(greedy) == 0);
    Owned smooth;
    REQUIRE(clay_voxel_mesh_smooth(grid, 0, &smooth.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(smooth) == 0);
}

TEST_CASE("c abi: a voxel target below the voxel size reports the clamp") {
    CGrid grid(0.1f, 15);
    clay_quad_params p = quad_defaults();
    p.target_quads = 4000000;  // finer than the grid itself holds
    Owned mesh;
    REQUIRE(clay_voxel_mesh_quads(grid, &p, &mesh.m) == CLAY_OK);

    clay_quad_report report{};
    report.struct_size = sizeof(clay_quad_report);
    REQUIRE(clay_mesh_quad_report(mesh, &report) == CLAY_OK);
    CHECK(report.clamped == 1);
    CHECK(report.within_tolerance == 0);
    CHECK(report.cell_size == doctest::Approx(0.1f));
    CHECK(report.quad_count == clay_mesh_quad_count(mesh));
    CHECK(report.quad_count > 0);
}

TEST_CASE("c abi: a level the grid does not have is not found") {
    CGrid grid(0.1f, 3);
    clay_quad_params p = quad_defaults();
    p.level = 4;
    clay_mesh* mesh = nullptr;
    CHECK(clay_voxel_mesh_quads(grid, &p, &mesh) == CLAY_ERROR_NOT_FOUND);
    CHECK(mesh == nullptr);
}

TEST_CASE("c abi: quads follow a transform and are dropped by a mixed concat") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.cell_size = 0.07f;
    Owned quads;
    REQUIRE(clay_document_mesh_quads(doc, &p, &quads.m) == CLAY_OK);
    const size_t before = clay_mesh_quad_count(quads);
    REQUIRE(before > 0);

    const float position[3] = {1, 2, 3}, axis[3] = {0, 1, 0};
    Owned moved;
    REQUIRE(clay_mesh_transform(quads, position, axis, 0.5f, 2.0f, &moved.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(moved) == before);
    check_invariant(moved);
    clay_quad_report report{};
    report.struct_size = sizeof(clay_quad_report);
    CHECK(clay_mesh_quad_report(moved, &report) == CLAY_OK);  // same mesh, moved

    clay_mesh_params mp{};
    mp.struct_size = sizeof(clay_mesh_params);
    mp.resolution = 24;
    Owned triangles;
    REQUIRE(clay_document_mesh(doc, &mp, &triangles.m) == CLAY_OK);

    // All-or-nothing, as for normals and uvs: a result that is quads over part
    // of itself would break the invariant every quad consumer reads.
    const clay_mesh* mixed_parts[2] = {quads, triangles};
    Owned mixed;
    REQUIRE(clay_mesh_concat(mixed_parts, 2, &mixed.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(mixed) == 0);
    CHECK(clay_mesh_index_count(mixed) ==
          clay_mesh_index_count(quads) + clay_mesh_index_count(triangles));
    CHECK(clay_mesh_quad_report(mixed, &report) == CLAY_ERROR_INVALID_ARGUMENT);

    const clay_mesh* both_parts[2] = {quads, moved};
    Owned both;
    REQUIRE(clay_mesh_concat(both_parts, 2, &both.m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(both) == before * 2);
    check_invariant(both);
}

TEST_CASE("c abi: a quad mesh layer keeps its quads") {
    Doc doc;
    clay_quad_params p = quad_defaults();
    p.cell_size = 0.08f;
    Owned quads;
    REQUIRE(clay_document_mesh_quads(doc, &p, &quads.m) == CLAY_OK);
    const size_t count = clay_mesh_quad_count(quads);
    REQUIRE(count > 0);

    clay_mesh_layer_desc desc{};
    desc.struct_size = sizeof(clay_mesh_layer_desc);
    desc.name = "carried";
    clay_layer_id layer = 0;
    clay_mesh* borrowed = nullptr;  // owned by the document, not destroyed here
    REQUIRE(clay_document_add_mesh_layer(doc, quads, &desc, &layer, &borrowed) == CLAY_OK);
    REQUIRE(borrowed != nullptr);
    CHECK(clay_mesh_quad_count(borrowed) == count);
    check_invariant(borrowed);
    for (size_t i = 0; i < count * 4; ++i)
        REQUIRE(clay_mesh_quads(borrowed)[i] == clay_mesh_quads(quads)[i]);

    // The geometry crossed; the report did not, because a mesh read out of a
    // document was not produced by a meshing call.
    clay_quad_report report{};
    report.struct_size = sizeof(clay_quad_report);
    CHECK(clay_mesh_quad_report(borrowed, &report) == CLAY_ERROR_INVALID_ARGUMENT);
}
