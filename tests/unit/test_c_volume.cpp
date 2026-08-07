#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
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
    REQUIRE(clay_mesh_load(path, nullptr, &loaded) == CLAY_OK);  // NULL = defaults
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
        CHECK(clay_mesh_load("model.xyzzy", nullptr, &nothing) == CLAY_ERROR_UNSUPPORTED);
        CHECK(nothing == nullptr);
    }
}

TEST_CASE("c volume: an imported shape can be smoothed") {
    // The workflow an app reaches for: bring in a scan, then smooth it. From
    // this ABI a volume comes from a mesh, so that is the shape relax gets.
    clay_mesh* mesh = build_box_mesh(0.6f);
    clay_volume_params params = volume_params(0.04f);
    clay_item* item = nullptr;
    REQUIRE(clay_item_volume_from_mesh(mesh, &params, &item) == CLAY_OK);

    clay_relax_params relax;
    std::memset(&relax, 0, sizeof relax);
    relax.struct_size = static_cast<std::uint32_t>(sizeof relax);
    relax.strength = 1.0f;
    relax.radius_cells = 2;
    relax.iterations = 2;
    REQUIRE(clay_item_volume_relax(item, &relax) == CLAY_OK);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    // Still a box, with its corners rounded off — which is what smoothing a
    // box does, and the reason to check a corner rather than a face.
    CHECK(eval_c(doc, cf3(0, 0, 0)) < 0.0f);
    CHECK(eval_c(doc, cf3(2.0f, 0, 0)) > 0.0f);
    CHECK(std::abs(eval_c(doc, cf3(0.6f, 0, 0))) < 0.08f);

    clay_item_destroy(item);
    clay_document_destroy(doc);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c volume: relaxing something that is not a volume is refused") {
    // Silently returning OK would look like it worked, and the caller would
    // wonder why nothing got smoother.
    const float radius[1] = {1.0f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(sphere != nullptr);

    clay_relax_params relax;
    std::memset(&relax, 0, sizeof relax);
    relax.struct_size = static_cast<std::uint32_t>(sizeof relax);
    relax.strength = 1.0f;
    CHECK(clay_item_volume_relax(sphere, &relax) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_volume_relax(nullptr, &relax) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_volume_relax(sphere, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    SUBCASE("and a descriptor without its struct_size is refused") {
        clay_relax_params blank;
        std::memset(&blank, 0, sizeof blank);
        CHECK(clay_item_volume_relax(sphere, &blank) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    clay_item_destroy(sphere);
}

TEST_CASE("c volume: an imported shape can be faceted") {
    // The verb SDF layers were missing. From this ABI a volume comes from a
    // mesh, so that is the shape flatten gets.
    clay_mesh* mesh = build_box_mesh(0.6f);
    clay_volume_params params = volume_params(0.04f);
    clay_item* item = nullptr;
    REQUIRE(clay_item_volume_from_mesh(mesh, &params, &item) == CLAY_OK);

    clay_flatten_params flat;
    std::memset(&flat, 0, sizeof flat);
    flat.struct_size = static_cast<std::uint32_t>(sizeof flat);
    flat.plane_point[1] = 0.45f;   // below the box's top face at 0.6
    flat.plane_normal[1] = 1.0f;
    flat.strength = 1.0f;
    // A region is required: flatten is local, and with none it would replace
    // the box with a half-space rather than facet it.
    flat.centre[1] = 0.6f;
    flat.region_radius = 0.5f;
    flat.falloff = 0.3f;
    // One call: the volume is re-sampled with the flatten applied, so the new
    // band brackets the facet rather than where the surface used to be.
    REQUIRE(clay_item_volume_flatten(item, &flat) == CLAY_OK);

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    CHECK(eval_c(doc, cf3(0, 0, 0)) < 0.0f);          // still solid inside
    CHECK(eval_c(doc, cf3(0, 0.58f, 0)) > 0.0f);      // the top was taken down
    CHECK(std::abs(eval_c(doc, cf3(0, 0.45f, 0))) < 0.07f);  // ...to about the plane

    clay_item_destroy(item);
    clay_document_destroy(doc);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c volume: flatten refuses what it cannot do") {
    const float radius[1] = {1.0f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(sphere != nullptr);

    clay_flatten_params flat;
    std::memset(&flat, 0, sizeof flat);
    flat.struct_size = static_cast<std::uint32_t>(sizeof flat);
    flat.plane_normal[1] = 1.0f;
    flat.region_radius = 0.5f;

    CHECK(clay_item_volume_flatten(sphere, &flat) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_volume_flatten(nullptr, &flat) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_volume_flatten(sphere, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(sphere);

    SUBCASE("and no region, which would replace the shape rather than facet it") {
        clay_mesh* mesh = build_box_mesh(0.5f);
        clay_volume_params params = volume_params(0.05f);
        clay_item* item = nullptr;
        REQUIRE(clay_item_volume_from_mesh(mesh, &params, &item) == CLAY_OK);
        clay_flatten_params global = flat;
        global.region_radius = 0.0f;
        CHECK(clay_item_volume_flatten(item, &global) == CLAY_ERROR_INVALID_ARGUMENT);
        clay_item_destroy(item);
        clay_mesh_destroy(mesh);
    }

    SUBCASE("and a zero normal, which describes no plane") {
        clay_mesh* mesh = build_box_mesh(0.5f);
        clay_volume_params params = volume_params(0.05f);
        clay_item* item = nullptr;
        REQUIRE(clay_item_volume_from_mesh(mesh, &params, &item) == CLAY_OK);
        clay_flatten_params zero = flat;
        zero.plane_normal[1] = 0.0f;
        CHECK(clay_item_volume_flatten(item, &zero) == CLAY_ERROR_INVALID_ARGUMENT);
        clay_item_destroy(item);
        clay_mesh_destroy(mesh);
    }
}

TEST_CASE("c volume: a document can be baked, which is what relax and flatten needed") {
    // Issue #7 items 2 and 5 were both left half-done by the same absence: the
    // C ABI could build a volume from a MESH but not from a document, so an app
    // could smooth an imported scan and not its own sculpt, and could not
    // consolidate a long edit list into one item.
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);
    const float radius[1] = {0.6f};
    clay_item* ball = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(clay_layer_add_item(doc, layer, ball, nullptr) == CLAY_OK);
    clay_item_destroy(ball);

    clay_volume_params params = volume_params(0.04f);
    clay_item* baked = nullptr;
    REQUIRE(clay_item_volume_from_document(doc, &params, nullptr, nullptr, &baked) == CLAY_OK);
    REQUIRE(baked != nullptr);

    clay_document* out = clay_document_create();
    clay_layer_id baked_layer = 0;
    REQUIRE(clay_add_sdf_layer(out, "baked", &baked_layer) == CLAY_OK);
    REQUIRE(clay_layer_add_item(out, baked_layer, baked, nullptr) == CLAY_OK);

    // The baked field is the document's field.
    CHECK(eval_c(out, cf3(0, 0, 0)) < 0.0f);
    CHECK(eval_c(out, cf3(2.0f, 0, 0)) > 0.0f);
    CHECK(std::abs(eval_c(out, cf3(0.6f, 0, 0))) < 0.05f);

    SUBCASE("and it can then be relaxed, which was the point") {
        clay_relax_params relax;
        std::memset(&relax, 0, sizeof relax);
        relax.struct_size = static_cast<std::uint32_t>(sizeof relax);
        relax.strength = 1.0f;
        relax.radius_cells = 2;
        relax.iterations = 2;
        CHECK(clay_item_volume_relax(baked, &relax) == CLAY_OK);
    }

    SUBCASE("and flattened") {
        clay_flatten_params flat;
        std::memset(&flat, 0, sizeof flat);
        flat.struct_size = static_cast<std::uint32_t>(sizeof flat);
        flat.plane_point[1] = 0.4f;
        flat.plane_normal[1] = 1.0f;
        flat.strength = 1.0f;
        flat.centre[1] = 0.5f;
        flat.region_radius = 0.4f;
        flat.falloff = 0.25f;
        CHECK(clay_item_volume_flatten(baked, &flat) == CLAY_OK);
    }

    clay_item_destroy(baked);
    clay_document_destroy(out);
    clay_document_destroy(doc);
}

TEST_CASE("c volume: baking a document refuses what it cannot do") {
    clay_volume_params params = volume_params(0.05f);
    clay_item* item = nullptr;

    CHECK(clay_item_volume_from_document(nullptr, &params, nullptr, nullptr, &item) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_document* empty = clay_document_create();
    CHECK(clay_item_volume_from_document(empty, &params, nullptr, nullptr, &item) ==
          CLAY_ERROR_INVALID_ARGUMENT);  // nothing to sample

    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(empty, "l", &layer) == CLAY_OK);
    const float radius[1] = {0.5f};
    clay_item* ball = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(clay_layer_add_item(empty, layer, ball, nullptr) == CLAY_OK);
    clay_item_destroy(ball);

    SUBCASE("a document has no intrinsic scale, so a cell size is required") {
        // Unlike a mesh, whose bounds give one. Guessing here would silently
        // pick a resolution the caller never chose, and the resolution IS the
        // shape after a bake.
        clay_volume_params guess = volume_params(0.0f);
        CHECK(clay_item_volume_from_document(empty, &guess, nullptr, nullptr, &item) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a region with no surface in it is refused rather than returning nothing") {
        const float lo[3] = {8.0f, 8.0f, 8.0f};
        const float hi[3] = {9.0f, 9.0f, 9.0f};
        CHECK(clay_item_volume_from_document(empty, &params, lo, hi, &item) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("an explicit region is honoured") {
        const float lo[3] = {-0.8f, -0.8f, -0.8f};
        const float hi[3] = {0.8f, 0.8f, 0.8f};
        clay_item* scoped = nullptr;
        CHECK(clay_item_volume_from_document(empty, &params, lo, hi, &scoped) == CLAY_OK);
        CHECK(scoped != nullptr);
        clay_item_destroy(scoped);
    }

    clay_document_destroy(empty);
}

TEST_CASE("c volume: the import budget is settable, and the extension is case-insensitive") {
    clay_mesh* built = build_box_mesh(0.5f);
    const char* upper = "c_volume_roundtrip.OBJ";
    REQUIRE(clay_mesh_save(built, "c_volume_roundtrip.obj") == CLAY_OK);
    // A file called MODEL.OBJ is an OBJ file. Python's loader always accepted
    // one; the C ABI refusing it was a plain bug.
    std::rename("c_volume_roundtrip.obj", upper);
    clay_mesh* loaded = nullptr;
    CHECK(clay_mesh_load(upper, nullptr, &loaded) == CLAY_OK);
    CHECK(loaded != nullptr);
    if (loaded) clay_mesh_destroy(loaded);

    SUBCASE("and a budget too small is refused before anything is allocated") {
        clay_import_budget tight;
        std::memset(&tight, 0, sizeof tight);
        tight.struct_size = static_cast<std::uint32_t>(sizeof tight);
        tight.max_vertices = 2;  // a box has eight
        clay_mesh* refused = nullptr;
        CHECK(clay_mesh_load(upper, &tight, &refused) == CLAY_ERROR_BUDGET_EXCEEDED);
        CHECK(refused == nullptr);
    }

    SUBCASE("and a generous one loads") {
        clay_import_budget roomy;
        std::memset(&roomy, 0, sizeof roomy);
        roomy.struct_size = static_cast<std::uint32_t>(sizeof roomy);
        roomy.max_vertices = 1000;
        roomy.max_triangles = 1000;
        clay_mesh* fine = nullptr;
        CHECK(clay_mesh_load(upper, &roomy, &fine) == CLAY_OK);
        if (fine) clay_mesh_destroy(fine);
    }

    SUBCASE("and a zeroed field means the default rather than allow-nothing") {
        clay_import_budget zeroed;
        std::memset(&zeroed, 0, sizeof zeroed);
        zeroed.struct_size = static_cast<std::uint32_t>(sizeof zeroed);
        clay_mesh* fine = nullptr;
        CHECK(clay_mesh_load(upper, &zeroed, &fine) == CLAY_OK);
        if (fine) clay_mesh_destroy(fine);
    }

    std::remove(upper);
    clay_mesh_destroy(built);
}
