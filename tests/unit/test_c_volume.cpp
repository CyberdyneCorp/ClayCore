#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/scene/tape.h"

// Building a volume through the C ABI (c-abi spec, add-mesh-to-field-import).
// add-sampled-fields declared CLAY_PRIM_VOLUME and refused to construct one
// because nothing could supply the samples; a mesh supplies them.

using namespace clay;
using kernel::cf3;

namespace {

float eval_c(clay_document* doc, kernel::cfloat3 p) {
    float point[3] = {p.x, p.y, p.z};
    float out = 0.0f;
    REQUIRE(clay_eval_points(doc, "cpu", point, 1, &out, nullptr) == CLAY_OK);
    return out;
}

// A closed box as raw triangles, wound outward.
struct BoxTriangles {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
};

BoxTriangles box_triangles(float h) {
    BoxTriangles b;
    for (int i = 0; i < 8; ++i) {
        b.positions.push_back((i & 1) ? h : -h);
        b.positions.push_back((i & 2) ? h : -h);
        b.positions.push_back((i & 4) ? h : -h);
    }
    const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                             {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        b.indices.insert(b.indices.end(), {static_cast<std::uint32_t>(f[0]),
                                           static_cast<std::uint32_t>(f[1]),
                                           static_cast<std::uint32_t>(f[2])});
        b.indices.insert(b.indices.end(), {static_cast<std::uint32_t>(f[0]),
                                           static_cast<std::uint32_t>(f[2]),
                                           static_cast<std::uint32_t>(f[3])});
    }
    return b;
}

clay_mesh* build_box_mesh(float h) {
    BoxTriangles b = box_triangles(h);
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(b.positions.data(), b.positions.size() / 3, b.indices.data(),
                                     b.indices.size(), &mesh) == CLAY_OK);
    return mesh;
}

clay_volume_params volume_params(float cell) {
    clay_volume_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.cell_size = cell;
    return p;
}

}  // namespace

TEST_CASE("c volume: a mesh becomes an item") {
    clay_mesh* mesh = build_box_mesh(0.6f);
    clay_volume_params params = volume_params(0.05f);
    clay_item* item = nullptr;
    REQUIRE(clay_item_volume_from_mesh(mesh, &params, &item) == CLAY_OK);
    REQUIRE(item != nullptr);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    CHECK(eval_c(doc, cf3(0, 0, 0)) < 0.0f);        // inside the box
    CHECK(eval_c(doc, cf3(2.0f, 0, 0)) > 0.0f);     // well outside it
    // On the face, where the sampling is a real distance: within a cell of
    // zero. An absolute bound, since a relative one around zero means nothing.
    CHECK(std::abs(eval_c(doc, cf3(0.6f, 0, 0))) < 0.06f);

    SUBCASE("and survives the round trip") {
        std::vector<kernel::cfloat3> probes = {cf3(0, 0, 0), cf3(0.5f, 0.2f, 0.1f),
                                               cf3(0.62f, 0, 0), cf3(1.5f, 0, 0)};
        std::vector<float> before;
        for (kernel::cfloat3 p : probes) before.push_back(eval_c(doc, p));

        const char* path = "c_volume_roundtrip.clayspace";
        REQUIRE(clay_document_save(doc, path) == CLAY_OK);
        clay_document* back = nullptr;
        REQUIRE(clay_document_load(path, &back) == CLAY_OK);

        for (std::size_t i = 0; i < probes.size(); ++i)
            CHECK(eval_c(back, probes[i]) == doctest::Approx(before[i]));
        clay_document_destroy(back);
    }

    clay_item_destroy(item);
    clay_document_destroy(doc);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c volume: clay_item_create still will not build one") {
    // It has no samples to give, so an item built there could only ever be
    // silently empty. The refusal names the producer.
    const float ignored[1] = {0.0f};
    CHECK(clay_item_create(CLAY_PRIM_VOLUME, ignored, 1) == nullptr);
    CHECK(clay_item_create(CLAY_PRIM_VOLUME, nullptr, 0) == nullptr);
}

TEST_CASE("c volume: degenerate input is refused where the item is built") {
    clay_volume_params params = volume_params(0.05f);
    clay_item* item = nullptr;

    CHECK(clay_item_volume_from_mesh(nullptr, &params, &item) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_mesh* mesh = build_box_mesh(0.5f);
    CHECK(clay_item_volume_from_mesh(mesh, nullptr, &item) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_volume_from_mesh(mesh, &params, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    SUBCASE("a mesh with no triangles has no surface to measure from") {
        clay_mesh* empty = nullptr;
        const float one[3] = {0, 0, 0};
        const std::uint32_t none[3] = {0, 0, 0};
        // No indices at all is refused at construction...
        CHECK(clay_mesh_from_triangles(one, 1, none, 0, &empty) == CLAY_ERROR_INVALID_ARGUMENT);
        // ...as is a partial triangle.
        CHECK(clay_mesh_from_triangles(one, 1, none, 2, &empty) == CLAY_ERROR_INVALID_ARGUMENT);
        // ...as is an index that points past the vertices.
        const std::uint32_t past[3] = {0, 1, 7};
        CHECK(clay_mesh_from_triangles(one, 1, past, 3, &empty) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a descriptor without its struct_size is refused") {
        clay_volume_params blank;
        std::memset(&blank, 0, sizeof blank);
        CHECK(clay_item_volume_from_mesh(mesh, &blank, &item) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a zero cell size picks one rather than failing") {
        // A default in world units would be far too fine for a building and
        // far too coarse for a bolt, so it comes from the mesh's own size.
        clay_volume_params automatic = volume_params(0.0f);
        clay_item* sized = nullptr;
        CHECK(clay_item_volume_from_mesh(mesh, &automatic, &sized) == CLAY_OK);
        CHECK(sized != nullptr);
        clay_item_destroy(sized);
    }

    clay_mesh_destroy(mesh);
}

TEST_CASE("c volume: an older descriptor still works") {
    // The prefix rule: a caller compiled before a field existed declares the
    // smaller struct_size and the rest zero-fills, which is what "0 means pick
    // one for me" is for.
    struct OldParams {
        std::uint32_t struct_size;
        float cell_size;
        float band;
        float padding;
        float beta;
    };
    static_assert(sizeof(OldParams) == sizeof(clay_volume_params));

    clay_mesh* mesh = build_box_mesh(0.5f);
    OldParams old{};
    old.struct_size = sizeof(OldParams);
    old.cell_size = 0.06f;
    clay_item* item = nullptr;
    CHECK(clay_item_volume_from_mesh(mesh, reinterpret_cast<const clay_volume_params*>(&old),
                                     &item) == CLAY_OK);
    CHECK(item != nullptr);
    clay_item_destroy(item);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c volume: a mesh loads from a file and imports") {
    // The counterpart to clay_mesh_save, and what gives an app something to
    // sample in the first place.
    clay_mesh* built = build_box_mesh(0.55f);
    const char* path = "c_volume_roundtrip.obj";
    REQUIRE(clay_mesh_save(built, path) == CLAY_OK);

    clay_mesh* loaded = nullptr;
    REQUIRE(clay_mesh_load(path, &loaded) == CLAY_OK);
    REQUIRE(loaded != nullptr);
    CHECK(clay_mesh_index_count(loaded) == clay_mesh_index_count(built));

    clay_volume_params params = volume_params(0.05f);
    clay_item* item = nullptr;
    REQUIRE(clay_item_volume_from_mesh(loaded, &params, &item) == CLAY_OK);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);
    CHECK(eval_c(doc, cf3(0, 0, 0)) < 0.0f);
    CHECK(eval_c(doc, cf3(2.0f, 0, 0)) > 0.0f);

    clay_item_destroy(item);
    clay_document_destroy(doc);
    clay_mesh_destroy(loaded);
    clay_mesh_destroy(built);

    SUBCASE("an unknown extension is reported rather than guessed at") {
        clay_mesh* nothing = nullptr;
        CHECK(clay_mesh_load("model.xyzzy", &nothing) == CLAY_ERROR_UNSUPPORTED);
        CHECK(nothing == nullptr);
    }
}
