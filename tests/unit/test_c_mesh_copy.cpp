#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// Caller-owned interleaved mesh copy-out (c-abi spec: a mesh can be copied into
// a caller's own vertex layout). Issue #43 item 4: the mesh accessors borrow
// into four separate engine-owned arrays, so a GPU host pays an interleave pass
// into a staging vector plus a copy into the mapped buffer — two passes over
// geometry that was just produced, on the frame path. This is the one pass, in
// the host's layout, into the host's memory.
//
// The interesting cases are the refusals: a layout naming an attribute the mesh
// does not carry, overlapping attributes, and a stride that does not clear
// them. All three produce a buffer that is wrong without looking wrong, so all
// three are errors rather than best-effort.

namespace {

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

struct MeshHandle {
    clay_mesh* m = nullptr;
    ~MeshHandle() { clay_mesh_destroy(m); }
    MeshHandle() = default;
    MeshHandle(const MeshHandle&) = delete;
    MeshHandle& operator=(const MeshHandle&) = delete;
};

struct Cache {
    clay_brick_cache* c = nullptr;
    Cache() {
        clay_brick_config cfg;
        cfg.struct_size = sizeof(cfg);
        REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
        c = clay_brick_cache_create(&cfg);
        REQUIRE(c != nullptr);
    }
    ~Cache() { clay_brick_cache_destroy(c); }
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
};

// The brick mesher rather than clay_document_mesh, because it is the one that
// lets a test choose which attributes exist: `with_field` gives gradient
// normals and colours, and its absence gives positions and face normals only —
// which is the mesh the "an attribute the mesh does not carry" refusals need.
void mesh_sphere(Doc& doc, Cache& cache, bool with_field, MeshHandle* out) {
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache.c, doc.d, doc.layer) == CLAY_OK);
    constexpr std::size_t kChunk = 64;
    constexpr std::size_t kSamples = 8 * 8 * 8;
    std::vector<clay_brick_request> reqs(kChunk);
    std::vector<float> values(kChunk * kSamples);
    std::vector<std::int32_t> results(kChunk);
    for (;;) {
        std::size_t count = kChunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache.c, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache.c, reqs.data(), count, values.data(),
                                        count * kSamples, nullptr, 0, results.data(),
                                        &accepted) == CLAY_OK);
        if (remaining == 0) break;
    }
    clay_brick_mesh_params p{};
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.normals = with_field ? CLAY_NORMAL_GRADIENT : CLAY_NORMAL_FACE;
    p.colors = with_field ? 1 : 0;
    p.gradient_eps = 0.0f;
    REQUIRE(clay_brick_cache_mesh(cache.c, with_field ? doc.d : nullptr, &p, nullptr, 0, nullptr,
                                  &out->m) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(out->m) > 0);
}

clay_vertex_layout layout_of(std::int32_t pos, std::int32_t nrm, std::int32_t col,
                             std::int32_t uv, std::uint32_t stride) {
    clay_vertex_layout l{};
    l.struct_size = static_cast<std::uint32_t>(sizeof l);
    l.stride = stride;
    l.position_offset = pos;
    l.normal_offset = nrm;
    l.color_offset = col;
    l.uv_offset = uv;
    return l;
}

float at(const std::vector<std::uint8_t>& buf, std::size_t byte) {
    float v = 0.0f;
    std::memcpy(&v, buf.data() + byte, sizeof v);
    return v;
}

}  // namespace

TEST_CASE("mesh copy: one pass into the caller's own layout") {
    Doc doc;
    Cache cache;
    MeshHandle mesh;
    mesh_sphere(doc, cache, true, &mesh);
    const std::size_t vertices = clay_mesh_vertex_count(mesh.m);
    const float* pos = clay_mesh_positions(mesh.m);
    const float* nrm = clay_mesh_normals(mesh.m);
    const float* col = clay_mesh_colors(mesh.m);
    REQUIRE(nrm != nullptr);
    REQUIRE(col != nullptr);

    SUBCASE("tightly packed") {
        // stride 0 = the end of the last attribute named, which is well
        // defined only because the offsets are the caller's
        const clay_vertex_layout l = layout_of(0, 12, 24, -1, 0);
        std::vector<std::uint8_t> buf(vertices * 36);
        REQUIRE(clay_mesh_copy_vertices(mesh.m, &l, buf.data(), buf.size()) == CLAY_OK);
        for (std::size_t v = 0; v < vertices; ++v)
            for (int c = 0; c < 3; ++c) {
                CHECK(at(buf, v * 36 + 0 + c * 4) == pos[v * 3 + c]);
                CHECK(at(buf, v * 36 + 12 + c * 4) == nrm[v * 3 + c]);
                CHECK(at(buf, v * 36 + 24 + c * 4) == col[v * 3 + c]);
            }
    }

    SUBCASE("a padded stride and a reordered layout") {
        // colour first, position after a gap, normals last, padded to 64 —
        // an ordinary GPU vertex, and nothing here assumes an order
        const std::uint32_t stride = 64;
        const clay_vertex_layout l = layout_of(16, 48, 0, -1, stride);
        std::vector<std::uint8_t> buf(vertices * stride, 0xCD);
        REQUIRE(clay_mesh_copy_vertices(mesh.m, &l, buf.data(), buf.size()) == CLAY_OK);
        for (std::size_t v = 0; v < vertices; ++v) {
            for (int c = 0; c < 3; ++c) {
                CHECK(at(buf, v * stride + 0 + c * 4) == col[v * 3 + c]);
                CHECK(at(buf, v * stride + 16 + c * 4) == pos[v * 3 + c]);
                CHECK(at(buf, v * stride + 48 + c * 4) == nrm[v * 3 + c]);
            }
            // the padding the caller asked for is left alone, not zeroed
            for (std::size_t b = 28; b < 48; ++b) CHECK(buf[v * stride + b] == 0xCD);
        }
    }

    SUBCASE("indices, into the caller's buffer") {
        const std::size_t n = clay_mesh_index_count(mesh.m);
        std::vector<std::uint32_t> idx(n);
        REQUIRE(clay_mesh_copy_indices(mesh.m, idx.data(), idx.size()) == CLAY_OK);
        const std::uint32_t* src = clay_mesh_indices(mesh.m);
        for (std::size_t i = 0; i < n; ++i) CHECK(idx[i] == src[i]);
        CHECK(clay_mesh_copy_indices(mesh.m, idx.data(), n - 1) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_mesh_copy_indices(mesh.m, idx.data(), n + 1) == CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("mesh copy: every refusal") {
    Doc doc;
    Cache cache;
    MeshHandle plain;
    mesh_sphere(doc, cache, false, &plain);  // no colours, no uvs
    const std::size_t vertices = clay_mesh_vertex_count(plain.m);
    REQUIRE(vertices > 0);
    std::vector<std::uint8_t> buf(vertices * 64);

    SUBCASE("an attribute the mesh does not carry is refused, not invented") {
        // A silently black model is harder to diagnose than an error here.
        const clay_vertex_layout colours = layout_of(0, 12, 24, -1, 0);
        CHECK(clay_mesh_copy_vertices(plain.m, &colours, buf.data(), vertices * 36) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        const clay_vertex_layout uvs = layout_of(0, -1, -1, 12, 0);
        CHECK(clay_mesh_copy_vertices(plain.m, &uvs, buf.data(), vertices * 20) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("overlapping attributes") {
        const clay_vertex_layout l = layout_of(0, 8, -1, -1, 0);  // position ends at 12
        CHECK(clay_mesh_copy_vertices(plain.m, &l, buf.data(), vertices * 20) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a stride that does not clear the attributes") {
        const clay_vertex_layout l = layout_of(0, 12, -1, -1, 20);  // normals end at 24
        CHECK(clay_mesh_copy_vertices(plain.m, &l, buf.data(), vertices * 20) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a destination that is not exactly right") {
        const clay_vertex_layout l = layout_of(0, 12, -1, -1, 0);  // packed = 24
        CHECK(clay_mesh_copy_vertices(plain.m, &l, buf.data(), vertices * 24 - 1) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_mesh_copy_vertices(plain.m, &l, buf.data(), vertices * 24 + 1) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_mesh_copy_vertices(plain.m, &l, buf.data(), vertices * 24) == CLAY_OK);
    }

    SUBCASE("a layout naming nothing") {
        const clay_vertex_layout l = layout_of(-1, -1, -1, -1, 0);
        CHECK(clay_mesh_copy_vertices(plain.m, &l, buf.data(), 0) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("null arguments and a short struct_size") {
        const clay_vertex_layout l = layout_of(0, -1, -1, -1, 0);
        CHECK(clay_mesh_copy_vertices(nullptr, &l, buf.data(), vertices * 12) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_mesh_copy_vertices(plain.m, nullptr, buf.data(), vertices * 12) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_mesh_copy_vertices(plain.m, &l, nullptr, vertices * 12) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_vertex_layout stunted = l;
        stunted.struct_size = 4;
        CHECK(clay_mesh_copy_vertices(plain.m, &stunted, buf.data(), vertices * 12) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
}

// -- GLB import across the boundary (add-glb-import) --------------------------

TEST_CASE("c abi: clay_mesh_load reads a .glb it wrote") {
    // GLB was the only format clay_mesh_save could write that clay_mesh_load
    // could not read, which made a save/load pair fail on exactly the format a
    // host is most likely to hand it.
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    const float r = 0.5f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, it, &node) == CLAY_OK);
    clay_item_destroy(it);

    clay_mesh_params p{};
    p.struct_size = sizeof(p);
    p.resolution = 20;
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_document_mesh(doc, &p, &mesh) == CLAY_OK);

    const std::string path =
        (std::filesystem::temp_directory_path() / "c_abi_glb_roundtrip.glb").string();
    REQUIRE(clay_mesh_save(mesh, path.c_str()) == CLAY_OK);

    clay_mesh* back = nullptr;
    REQUIRE(clay_mesh_load(path.c_str(), nullptr, &back) == CLAY_OK);
    REQUIRE(back != nullptr);
    CHECK(clay_mesh_vertex_count(back) == clay_mesh_vertex_count(mesh));
    CHECK(clay_mesh_index_count(back) == clay_mesh_index_count(mesh));

    SUBCASE("and the budget is enforced across the boundary") {
        clay_mesh* refused = nullptr;
        clay_import_budget budget{};
        budget.struct_size = sizeof(budget);
        budget.max_vertices = 2;
        CHECK(clay_mesh_load(path.c_str(), &budget, &refused) == CLAY_ERROR_BUDGET_EXCEEDED);
        CHECK(refused == nullptr);
    }

    clay_mesh_destroy(back);
    clay_mesh_destroy(mesh);
    clay_document_destroy(doc);
}

// -- attribute transfer across the boundary (add-mesh-attribute-transfer) -----

TEST_CASE("c abi: attributes transfer between meshes, and the report is honest") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    const float r = 0.5f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, it, &node) == CLAY_OK);
    clay_item_destroy(it);

    clay_mesh_params p{};
    p.struct_size = sizeof(p);
    p.resolution = 24;
    clay_mesh* source = nullptr;
    REQUIRE(clay_document_mesh(doc, &p, &source) == CLAY_OK);
    p.resolution = 18;
    clay_mesh* target = nullptr;
    REQUIRE(clay_document_mesh(doc, &p, &target) == CLAY_OK);

    clay_transfer_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_transfer_defaults(&d) == CLAY_OK);
    CHECK(d.colors == 1);
    CHECK(d.normals == 0);  // off by default, and the default is the point

    const std::size_t n = clay_mesh_vertex_count(target);
    std::vector<float> before(n * 3);
    std::memcpy(before.data(), clay_mesh_positions(target), n * 3 * sizeof(float));

    clay_transfer_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_mesh_transfer_attributes(source, target, &d, &report) == CLAY_OK);
    CHECK(report.transferred > 0);
    CHECK(report.transferred + report.fell_back == n);
    CHECK(report.colors == 1);
    CHECK(report.max_distance > 0.0f);

    // A transfer, not a projection.
    CHECK(std::memcmp(before.data(), clay_mesh_positions(target), n * 3 * sizeof(float)) == 0);

    SUBCASE("a negative threshold is refused rather than defaulted") {
        clay_transfer_desc bad = d;
        bad.max_distance = -1.0f;
        CHECK(clay_mesh_transfer_attributes(source, target, &bad, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("the report descriptor is bounded like every other output") {
        clay_transfer_report old_layout{};
        old_layout.struct_size = offsetof(clay_transfer_report, max_distance) + sizeof(float);
        CHECK(clay_mesh_transfer_attributes(source, target, &d, &old_layout) == CLAY_OK);
        CHECK(old_layout.struct_size == offsetof(clay_transfer_report, max_distance) +
                                            sizeof(float));
    }

    clay_mesh_destroy(target);
    clay_mesh_destroy(source);
    clay_document_destroy(doc);
}
