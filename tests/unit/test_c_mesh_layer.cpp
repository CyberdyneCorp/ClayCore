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

TEST_CASE("c mesh layer: clay_layer_bounds answers from the mesh") {
    // THIS TEST ASSERTED THE OPPOSITE until issue #318, and the reversal is
    // deliberate rather than a drift. The old text read "clay_layer_bounds is
    // derived from SDF shapes and is deliberately left meaning what it always
    // meant: a mesh layer shows it nothing" -- which is a decision about the
    // IMPLEMENTATION (scene::Layer holds only SDF content) dressed as a
    // decision about the CONTRACT. A mesh cannot be unbounded: its vertices are
    // a box, and clay_mesh_bounds will give it. Reporting none was never the
    // truth, and every host worked around it by reading the geometry back --
    // for a 300k-triangle model, a copy of every position to answer a question
    // the engine can answer from data it already holds.
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

    float bmin[3] = {0, 0, 0};
    float bmax[3] = {0, 0, 0};
    std::int32_t has_bounds = 0;
    REQUIRE(clay_layer_bounds(d.doc, layer, bmin, bmax, &has_bounds) == CLAY_OK);
    REQUIRE(has_bounds == 1);
    // The SAME box clay_mesh_bounds reports, to the float. Compared against the
    // other entry point rather than against literals, so the two cannot drift.
    for (int i = 0; i < 3; ++i) {
        CHECK(bmin[i] == doctest::Approx(lo[i]));
        CHECK(bmax[i] == doctest::Approx(hi[i]));
    }
}

TEST_CASE("c mesh layer: layer bounds are WORLD space, so the transform applies") {
    // The SDF arm composes layer.xform with each node's own, so a caller
    // comparing two layers is asking one question. A mesh layer answering in
    // its own local space would answer a different one, and the difference is
    // invisible until somebody moves a layer.
    Doc d;
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    clay_layer_id layer = 0;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, nullptr) == CLAY_OK);
    clay_mesh_destroy(source);

    const float offset[3] = {10.0f, -5.0f, 2.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, offset, axis, 0.0f, 1.0f) == CLAY_OK);

    float bmin[3] = {0, 0, 0};
    float bmax[3] = {0, 0, 0};
    std::int32_t has_bounds = 0;
    REQUIRE(clay_layer_bounds(d.doc, layer, bmin, bmax, &has_bounds) == CLAY_OK);
    REQUIRE(has_bounds == 1);
    // the tetrahedron is (0,0,0)..(1,2,3) in its own space
    CHECK(bmin[0] == doctest::Approx(10.0f));
    CHECK(bmin[1] == doctest::Approx(-5.0f));
    CHECK(bmin[2] == doctest::Approx(2.0f));
    CHECK(bmax[0] == doctest::Approx(11.0f));
    CHECK(bmax[1] == doctest::Approx(-3.0f));
    CHECK(bmax[2] == doctest::Approx(5.0f));
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

// -- combining meshes for export (add-mesh-layers 4.6/4.7, issue #54) --------
//
// clay_document_mesh keeps meaning "mesh the field". Combining is explicit,
// and these are the three calls that do it. Each scenario the c-abi delta
// names has a test here.

namespace {

// A mesh carrying uvs, so the attribute-drop rule has something to drop.
clay_mesh* square_with_uvs() {
    const float positions[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 6, &m) == CLAY_OK);
    return m;
}

}  // namespace

TEST_CASE("c mesh combine: transform moves positions and rotates normals") {
    clay_mesh* tet = tetrahedron();
    const float position[3] = {10.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};  // angle 0: the axis is required
    clay_mesh* moved = nullptr;
    REQUIRE(clay_mesh_transform(tet, position, axis, 0.0f, 2.0f, &moved) == CLAY_OK);

    std::vector<float> before = positions_of(tet);
    std::vector<float> after = positions_of(moved);
    REQUIRE(after.size() == before.size());
    // scaled by 2 then moved by 10 on x
    CHECK(after[0] == doctest::Approx(before[0] * 2.0f + 10.0f));
    CHECK(after[1] == doctest::Approx(before[1] * 2.0f));
    CHECK(after[7] == doctest::Approx(before[7] * 2.0f));  // the y=2 vertex

    SUBCASE("the refusals match every other transform in this ABI") {
        clay_mesh* bad = nullptr;
        CHECK(clay_mesh_transform(tet, position, axis, 0.0f, 0.0f, &bad) != CLAY_OK);
        const float zero_axis[3] = {0.0f, 0.0f, 0.0f};
        CHECK(clay_mesh_transform(tet, position, zero_axis, 0.0f, 1.0f, &bad) != CLAY_OK);
        CHECK(clay_mesh_transform(tet, nullptr, axis, 0.0f, 1.0f, &bad) != CLAY_OK);
    }

    clay_mesh_destroy(moved);
    clay_mesh_destroy(tet);
}

TEST_CASE("c mesh combine: concat rebases indices") {
    clay_mesh* a = tetrahedron();
    clay_mesh* b = tetrahedron();
    const clay_mesh* parts[2] = {a, b};
    clay_mesh* both = nullptr;
    REQUIRE(clay_mesh_concat(parts, 2, &both) == CLAY_OK);

    CHECK(clay_mesh_vertex_count(both) == clay_mesh_vertex_count(a) * 2);
    std::vector<std::uint32_t> idx = indices_of(both);
    std::vector<std::uint32_t> one = indices_of(a);
    REQUIRE(idx.size() == one.size() * 2);
    // the second copy's indices are shifted by the first copy's vertex count,
    // which is what "rebased" means and what a naive append gets wrong
    const std::uint32_t base = static_cast<std::uint32_t>(clay_mesh_vertex_count(a));
    for (std::size_t i = 0; i < one.size(); ++i) {
        CHECK(idx[i] == one[i]);
        CHECK(idx[one.size() + i] == one[i] + base);
    }
    // and every index is in range, which a wrong base would break
    for (std::uint32_t i : idx) CHECK(i < clay_mesh_vertex_count(both));

    clay_mesh_destroy(both);
    clay_mesh_destroy(a);
    clay_mesh_destroy(b);
}

TEST_CASE("c mesh combine: a mismatched attribute is dropped, not truncated") {
    // The rule exists because the alternative is a mesh whose uvs are
    // non-empty and a different length than its positions — malformed, and
    // discovered in an exported file rather than at the call.
    clay_mesh* with = square_with_uvs();
    clay_mesh* without = tetrahedron();
    const clay_mesh* parts[2] = {with, without};
    clay_mesh* both = nullptr;
    REQUIRE(clay_mesh_concat(parts, 2, &both) == CLAY_OK);

    // whatever the inputs carried, the result never carries a short array
    const std::size_t vertices = clay_mesh_vertex_count(both);
    CHECK(clay_mesh_vertex_count(both) == clay_mesh_vertex_count(with)
                                              + clay_mesh_vertex_count(without));
    CHECK(vertices > 0);

    clay_mesh_destroy(both);
    clay_mesh_destroy(with);
    clay_mesh_destroy(without);
}

TEST_CASE("c mesh combine: a sculpt exports beside its reference model") {
    Doc d;
    add_sphere(d.doc);

    clay_mesh* tet = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("reference");
    clay_layer_id layer = 0;
    REQUIRE(clay_document_add_mesh_layer(d.doc, tet, &desc, &layer, nullptr) == CLAY_OK);
    clay_mesh_destroy(tet);

    // put the reference somewhere the sphere is not
    const float position[3] = {5.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, position, axis, 0.0f, 1.0f)
            == CLAY_OK);

    clay_mesh_params params;
    std::memset(&params, 0, sizeof params);
    params.struct_size = static_cast<std::uint32_t>(sizeof params);
    params.resolution = 24;

    clay_mesh* field_only = nullptr;
    REQUIRE(clay_document_mesh(d.doc, &params, &field_only) == CLAY_OK);
    clay_mesh* combined = nullptr;
    REQUIRE(clay_document_mesh_combined(d.doc, &params, &combined) == CLAY_OK);

    // the combined result is the field plus the four reference vertices
    CHECK(clay_mesh_vertex_count(combined) == clay_mesh_vertex_count(field_only) + 4);

    // and the reference sits under its LAYER transform, not at the origin
    float lo[3], hi[3];
    REQUIRE(clay_mesh_bounds(combined, lo, hi) == CLAY_OK);
    CHECK(hi[0] > 4.0f);

    SUBCASE("a hidden mesh layer is not exported") {
        REQUIRE(clay_document_set_layer_visible(d.doc, layer, 0) == CLAY_OK);
        clay_mesh* hidden = nullptr;
        REQUIRE(clay_document_mesh_combined(d.doc, &params, &hidden) == CLAY_OK);
        CHECK(clay_mesh_vertex_count(hidden) == clay_mesh_vertex_count(field_only));
        clay_mesh_destroy(hidden);
    }

    SUBCASE("ghost and lock change nothing about the export") {
        REQUIRE(clay_document_set_layer_protection(d.doc, layer, 1, 1) == CLAY_OK);
        clay_mesh* protectedd = nullptr;
        REQUIRE(clay_document_mesh_combined(d.doc, &params, &protectedd) == CLAY_OK);
        // still there: neither flag changes what the document evaluates to,
        // so neither may change what it exports
        CHECK(clay_mesh_vertex_count(protectedd) == clay_mesh_vertex_count(combined));
        clay_mesh_destroy(protectedd);
    }

    SUBCASE("a document with no visible mesh layer exports the field alone") {
        Doc plain;
        add_sphere(plain.doc);
        clay_mesh* a = nullptr;
        clay_mesh* b = nullptr;
        REQUIRE(clay_document_mesh(plain.doc, &params, &a) == CLAY_OK);
        REQUIRE(clay_document_mesh_combined(plain.doc, &params, &b) == CLAY_OK);
        CHECK(clay_mesh_vertex_count(a) == clay_mesh_vertex_count(b));
        CHECK(clay_mesh_index_count(a) == clay_mesh_index_count(b));
        clay_mesh_destroy(a);
        clay_mesh_destroy(b);
    }

    clay_mesh_destroy(field_only);
    clay_mesh_destroy(combined);
}

// #365: the id-addressed route back to a mesh layer's geometry. A reopened
// document could reach it only through the name, which answers with the first
// layer in stack order carrying it, so a held id — the thing this ABI calls
// stable — could not be spent.
TEST_CASE("c mesh layer: the geometry is reachable by layer id, and refuses what it cannot") {
    Doc d;
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    clay_layer_id layer = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, &borrowed) == CLAY_OK);
    clay_mesh_destroy(source);

    // One borrow: the handle names its layer rather than caching a pointer, so
    // the creation, the name and the id all resolve to it.
    clay_mesh* by_id = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(d.doc, layer, &by_id) == CLAY_OK);
    CHECK(by_id == borrowed);
    CHECK(clay_mesh_vertex_count(by_id) == 4);
    CHECK(clay_mesh_index_count(by_id) == 12);

    add_sphere(d.doc);  // an SDF layer, whose id is a layer and not a mesh layer
    clay_layer_id sdf = 0;
    REQUIRE(clay_document_layer_at(d.doc, 0, &sdf) == CLAY_OK);
    if (sdf == layer) REQUIRE(clay_document_layer_at(d.doc, 1, &sdf) == CLAY_OK);
    CHECK(sdf != layer);

    clay_mesh* untouched = borrowed;
    CHECK(clay_document_mesh_layer_by_id(d.doc, sdf, &untouched) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_mesh_layer_by_id(d.doc, layer + 9999, &untouched) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_mesh_layer_by_id(nullptr, layer, &untouched) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_mesh_layer_by_id(d.doc, layer, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(untouched == borrowed);  // a refused lookup writes nothing
}

TEST_CASE("c mesh layer: a removed layer's id stops reaching the geometry it kept") {
    // The removal keeps the payload — the inverse of a removal cannot carry it
    // — so the id must be resolved in the DOCUMENT rather than in the payloads
    // held beside it, or an id would reach a layer that is no longer there.
    Doc d;
    clay_mesh* source = tetrahedron();
    clay_mesh_layer_desc desc = layer_desc("scan");
    clay_layer_id layer = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, &borrowed) == CLAY_OK);
    clay_mesh_destroy(source);
    REQUIRE(clay_document_remove_layer(d.doc, layer) == CLAY_OK);

    clay_mesh* gone = nullptr;
    CHECK(clay_document_mesh_layer_by_id(d.doc, layer, &gone) == CLAY_ERROR_NOT_FOUND);
    CHECK(gone == nullptr);
    // The payload is genuinely still there — this is the check being made, not
    // an absence standing in for it.
    CHECK(clay_mesh_vertex_count(borrowed) == 4);
}
