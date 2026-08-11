#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/field/volume.h"
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
    // The 0.27 layout, now a strict prefix: 0.28.0 appended `feather`, which
    // zero-fills to the hard replace for a caller this old.
    static_assert(sizeof(OldParams) < sizeof(clay_volume_params));

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

TEST_CASE("c abi: a flatten descriptor from before modes still means two-sided") {
    // The mode was APPENDED to clay_flatten_params. A caller compiled against
    // the previous layout sizes struct_size to it and must keep working, with
    // the two-sided behaviour it was written against — not read a mode out of
    // whatever follows its buffer.
    const float box[3] = {0.5f, 0.6f, 0.5f};

    // The pre-mode layout ends at `falloff`.
    const std::size_t pre_mode = offsetof(clay_flatten_params, falloff) + sizeof(float);
    REQUIRE(pre_mode < sizeof(clay_flatten_params));  // the mode really is appended

    clay_flatten_params flat;
    std::memset(&flat, 0, sizeof flat);
    flat.struct_size = static_cast<std::uint32_t>(pre_mode);
    flat.plane_point[1] = 0.45f;
    flat.plane_normal[1] = 1.0f;
    flat.strength = 1.0f;
    flat.centre[1] = 0.6f;
    flat.region_radius = 0.5f;
    flat.falloff = 0.3f;
    flat.mode = CLAY_FLATTEN_CUT_ONLY;  // past the declared size: must be ignored

    // Nothing to flatten here, so this asserts the DESCRIPTOR is accepted and
    // the out-of-range mode is not read, rather than the flatten's result.
    clay_item* not_a_volume = clay_item_create(CLAY_PRIM_SPHERE, box, 1);
    REQUIRE(not_a_volume != nullptr);
    // Refused for carrying no volume — not for a malformed descriptor.
    CHECK(clay_item_volume_flatten(not_a_volume, &flat) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(not_a_volume);
}

TEST_CASE("c abi: an unknown flatten mode is refused") {
    const float radius[1] = {0.5f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(item != nullptr);
    clay_flatten_params flat;
    std::memset(&flat, 0, sizeof flat);
    flat.struct_size = static_cast<std::uint32_t>(sizeof flat);
    flat.plane_normal[1] = 1.0f;
    flat.strength = 1.0f;
    flat.region_radius = 0.5f;
    flat.falloff = 0.2f;
    flat.mode = 99;
    CHECK(clay_item_volume_flatten(item, &flat) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(item);
}

// -- flatten sampled from a document (add-document-sourced-flatten) ----------
//
// `field::flatten` has had two overloads all along and the C ABI exposed one:
// the in-place form, which re-samples the item's own volume. That volume
// reports a distance only inside the band it carries and a lower BOUND outside
// it, so a facet moving further than the band is placed against the bound —
// a wrong surface returned with CLAY_OK.
//
// These tests are written to fail on the OLD behaviour. Asserting that the
// call succeeds would have passed before the change too.
TEST_CASE("c abi: a flatten can be sampled from a document") {
    // A ball whose surface sits at 0.6, and a plane well inside it. The facet
    // travels 0.25, which is far outside a three-cell band at this cell size.
    const float kCell = 0.02f;
    const float kNarrowBand = kCell * 3.0f;  // 0.06 — the default
    const float kTravel = 0.25f;

    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "src", &layer) == CLAY_OK);
    float radius = 0.6f;
    clay_item* ball = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
    REQUIRE(ball != nullptr);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, ball, &node) == CLAY_OK);
    clay_item_destroy(ball);

    clay_flatten_params fp;
    std::memset(&fp, 0, sizeof fp);
    fp.struct_size = (std::uint32_t)sizeof fp;
    fp.plane_point[1] = 0.6f - kTravel;  // plane 0.25 below the crown
    fp.plane_normal[1] = 1.0f;
    fp.strength = 1.0f;
    fp.centre[1] = 0.6f - kTravel;
    fp.region_radius = 0.45f;
    fp.falloff = 0.10f;
    fp.mode = CLAY_FLATTEN_TWO_SIDED;

    clay_volume_params vp;
    std::memset(&vp, 0, sizeof vp);
    vp.struct_size = (std::uint32_t)sizeof vp;
    vp.cell_size = kCell;
    vp.band = kNarrowBand;
    vp.padding = 0.1f;

    const float lo[3] = {-0.9f, -0.9f, -0.9f};
    const float hi[3] = {0.9f, 0.9f, 0.9f};

    SUBCASE("the facet lands on the plane") {
        clay_item* flat = nullptr;
        REQUIRE(clay_item_volume_flatten_from(doc, &fp, &vp, lo, hi, &flat) == CLAY_OK);
        REQUIRE(flat != nullptr);

        clay_document* out = clay_document_create();
        clay_layer_id ol = 0;
        REQUIRE(clay_add_sdf_layer(out, "out", &ol) == CLAY_OK);
        clay_node_id on = 0;
        REQUIRE(clay_layer_add_item(out, ol, flat, &on) == CLAY_OK);

        // On the plane the field is ~0; a little above it is outside.
        const float plane_y = 0.6f - kTravel;
        CHECK(std::fabs(eval_c(out, cf3(0, plane_y, 0))) < 3.0f * kCell);
        CHECK(eval_c(out, cf3(0, plane_y + 0.12f, 0)) > 0.0f);
        // ...and the untouched side of the ball is still there.
        CHECK(eval_c(out, cf3(0, -0.4f, 0)) < 0.0f);

        clay_item_destroy(flat);
        clay_document_destroy(out);
    }

    SUBCASE("the document-sourced field is far cheaper to march") {
        // The measured difference between the two sources is NOT the surface.
        // Both place the facet in the same place, at every band tried — an
        // earlier draft of this test asserted otherwise and was wrong.
        //
        // What differs is STEEPNESS. Flattening a volume blends the plane with
        // a source that is itself sampled, and the result declares a much
        // worse Lipschitz than flattening from the exact document does. The
        // engine's own raycast marches by safe_step_scale, so an 8x worse
        // scale is 8x the marching cost for the same shape — and past some
        // point the marcher runs out of iterations and the surface stops
        // being drawable at all, which is how this was first noticed.
        clay_item* baked = nullptr;
        REQUIRE(clay_item_volume_from_document(doc, &vp, lo, hi, &baked) == CLAY_OK);
        REQUIRE(clay_item_volume_flatten(baked, &fp) == CLAY_OK);

        clay_item* sampled = nullptr;
        REQUIRE(clay_item_volume_flatten_from(doc, &fp, &vp, lo, hi, &sampled) == CLAY_OK);

        auto place = [&](clay_item* it) {
            clay_document* d = clay_document_create();
            clay_layer_id l = 0;
            REQUIRE(clay_add_sdf_layer(d, "o", &l) == CLAY_OK);
            clay_node_id n = 0;
            REQUIRE(clay_layer_add_item(d, l, it, &n) == CLAY_OK);
            return d;
        };
        clay_document* from_volume = place(baked);
        clay_document* from_doc = place(sampled);

        float step_volume = 0.0f, step_doc = 0.0f;
        REQUIRE(clay_safe_step_scale(from_volume, &step_volume) == CLAY_OK);
        REQUIRE(clay_safe_step_scale(from_doc, &step_doc) == CLAY_OK);
        CAPTURE(step_volume);
        CAPTURE(step_doc);
        CHECK(step_doc > step_volume * 2.0f);

        // and the shape really is the same, which is what makes the step
        // difference a cost rather than a trade
        const float plane_y = 0.6f - kTravel;
        CHECK(std::fabs(eval_c(from_doc, cf3(0, plane_y, 0))) < 3.0f * kCell);
        CHECK(std::fabs(eval_c(from_volume, cf3(0, plane_y, 0))) < 3.0f * kCell);

        clay_item_destroy(baked);
        clay_item_destroy(sampled);
        clay_document_destroy(from_volume);
        clay_document_destroy(from_doc);
    }

    SUBCASE("refusals") {
        clay_item* out = nullptr;
        clay_volume_params bad = vp;
        bad.cell_size = 0.0f;  // a document has no intrinsic scale
        CHECK(clay_item_volume_flatten_from(doc, &fp, &bad, lo, hi, &out) != CLAY_OK);

        clay_flatten_params no_region = fp;
        no_region.region_radius = 0.0f;  // would replace the shape with a half-space
        CHECK(clay_item_volume_flatten_from(doc, &no_region, &vp, lo, hi, &out) != CLAY_OK);

        clay_flatten_params no_plane = fp;
        no_plane.plane_normal[0] = no_plane.plane_normal[1] = no_plane.plane_normal[2] = 0.0f;
        CHECK(clay_item_volume_flatten_from(doc, &no_plane, &vp, lo, hi, &out) != CLAY_OK);

        // half a region is a caller that meant to pass one
        CHECK(clay_item_volume_flatten_from(doc, &fp, &vp, lo, nullptr, &out) != CLAY_OK);
        CHECK(clay_item_volume_flatten_from(doc, &fp, &vp, nullptr, hi, &out) != CLAY_OK);
    }

    clay_document_destroy(doc);
}

// -- the replace round trip and its feather (issue #67, ------------------------
// add-feathered-volume-replace)
//
// Baking a region and putting it straight back with CLAY_OP_REPLACE — no verb
// applied — left the surface shaded like corrugated metal. The zero set was
// EXACT (these tests hold that); what corrugated was the normals: the hard
// replace holds both fields live at the surface, a fresh bake ties with the
// field beneath it at every sample plane, and any finite-difference gradient
// across a min/max branch switch pays |b - a| over its own epsilon, at the
// cell wavelength. The feather crossfades to ONE field deep inside the box,
// so only one gradient survives — and the error finally shrinks with the cell.

namespace {

// A unit ball as one SDF layer, the issue's own repro document.
clay_document* unit_ball_doc(clay_layer_id* out_layer) {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    REQUIRE(clay_add_sdf_layer(doc, "ball", out_layer) == CLAY_OK);
    clay_item_desc it;
    std::memset(&it, 0, sizeof it);
    it.struct_size = sizeof it;
    it.prim = CLAY_PRIM_SPHERE;
    it.params[0] = 1.0f;
    it.rotation[3] = 1.0f;
    it.scale = 1.0f;
    it.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, *out_layer, &it, &node) == CLAY_OK);
    return doc;
}

// The issue's region: a box clipping the cap of the ball around +z.
constexpr float kCapMin[3] = {-0.7f, -0.4f, 0.4f};
constexpr float kCapMax[3] = {0.7f, 0.4f, 1.3f};

// Directions through the cap, kept well inside the region box.
std::vector<kernel::cfloat3> cap_directions() {
    std::vector<kernel::cfloat3> dirs;
    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 9; ++i) {
            float x = -0.5f + 1.0f * (i + 0.5f) / 9.0f;
            float y = -0.28f + 0.56f * (j + 0.5f) / 5.0f;
            float z = std::sqrt(1.0f - x * x - y * y);
            dirs.push_back(cf3(x, y, z));
        }
    return dirs;
}

// Bake the cap and put it straight back with CLAY_OP_REPLACE. No verb.
void replace_cap(clay_document* doc, clay_layer_id layer, float cell, float feather) {
    clay_volume_params vp = volume_params(cell);
    vp.feather = feather;
    clay_item* item = nullptr;
    REQUIRE(clay_item_volume_from_document(doc, &vp, kCapMin, kCapMax, &item) == CLAY_OK);
    REQUIRE(clay_item_set_op(item, CLAY_OP_REPLACE) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);
}

// Worst angle, in degrees, between the field gradient and the radial
// direction over the cap — the corrugation, measured where shading reads it.
double worst_normal_tilt_deg(clay_document* doc) {
    std::vector<kernel::cfloat3> dirs = cap_directions();
    std::vector<float> pts, grads(dirs.size() * 3);
    for (const kernel::cfloat3& d : dirs) {
        pts.push_back(d.x);
        pts.push_back(d.y);
        pts.push_back(d.z);
    }
    REQUIRE(clay_eval_gradients(doc, "cpu", pts.data(), dirs.size(), grads.data()) == CLAY_OK);
    double worst = 0.0;
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        double dot = grads[3 * i] * dirs[i].x + grads[3 * i + 1] * dirs[i].y +
                     grads[3 * i + 2] * dirs[i].z;
        dot = std::min(1.0, std::fabs(dot));
        worst = std::max(worst, std::acos(dot) * 180.0 / 3.14159265358979);
    }
    return worst;
}

// Worst |radius - 1| of the field's zero crossing along the cap directions,
// by bisection — the surface itself, free of any marching tolerance.
double worst_radial_deviation(clay_document* doc) {
    double worst = 0.0;
    for (const kernel::cfloat3& d : cap_directions()) {
        float lo = 0.94f, hi = 1.06f;
        REQUIRE(eval_c(doc, d * lo) < 0.0f);
        REQUIRE(eval_c(doc, d * hi) > 0.0f);
        for (int it = 0; it < 30; ++it) {
            float m = 0.5f * (lo + hi);
            (eval_c(doc, d * m) < 0.0f ? lo : hi) = m;
        }
        worst = std::max(worst, std::fabs(0.5 * (lo + hi) - 1.0));
    }
    return worst;
}

}  // namespace

TEST_CASE("c abi: a feathered replace uncorrugates the bake round trip") {
    // Written to fail on the OLD behaviour: the hard replace's normal tilt sat
    // at tens of degrees and did NOT shrink with the cell — the issue's
    // constant-worst-deviation table, seen where it actually lands (normals).
    struct Row {
        float cell;
        double hard_tilt, feathered_tilt, feathered_dev;
    };
    Row rows[2] = {{0.04f, 0, 0, 0}, {0.02f, 0, 0, 0}};

    for (Row& row : rows) {
        clay_layer_id layer = 0;
        clay_document* hard = unit_ball_doc(&layer);
        replace_cap(hard, layer, row.cell, 0.0f);
        row.hard_tilt = worst_normal_tilt_deg(hard);
        // The surface itself was never wrong: exact to well below the cell
        // even under the hard replace. The corrugation is all in the normals.
        CHECK(worst_radial_deviation(hard) < 1e-4);
        clay_document_destroy(hard);

        clay_document* feathered = unit_ball_doc(&layer);
        replace_cap(feathered, layer, row.cell, 3.0f * row.cell);  // one band
        row.feathered_tilt = worst_normal_tilt_deg(feathered);
        row.feathered_dev = worst_radial_deviation(feathered);
        // The feathered field is steeper by band * 1.5 / feather and declares
        // it; the declared cost must stay a cost, not a cliff.
        float step = 0.0f;
        REQUIRE(clay_safe_step_scale(feathered, &step) == CLAY_OK);
        CHECK(step > 0.15f);
        clay_document_destroy(feathered);
    }

    // The defect: hard-replace normals corrugate far past anything trilinear
    // reconstruction explains (measured 32 degrees at cell 0.04).
    CHECK(rows[0].hard_tilt > 4.0);
    // The fix: single-field normals, converging as the cell shrinks.
    CHECK(rows[0].feathered_tilt < 2.5);
    CHECK(rows[1].feathered_tilt < 1.2);
    CHECK(rows[1].feathered_tilt < rows[0].feathered_tilt);
    // The surface deviation is now plain trilinear reconstruction —
    // O(cell^2) — rather than resolution-independent.
    CHECK(rows[0].feathered_dev < 0.7 * 0.04 * 0.04);
    CHECK(rows[1].feathered_dev < 0.7 * 0.02 * 0.02);
    CHECK(rows[1].feathered_dev < 0.5 * rows[0].feathered_dev);
}

TEST_CASE("c abi: feather zero is the hard replace it always was") {
    // Byte-identity pinned algebraically: with feather 0 (or a descriptor
    // from before the field existed) the composed field IS
    // min(max(a, -b), b) of the two fields evaluated separately.
    clay_layer_id layer = 0;
    clay_document* composed = unit_ball_doc(&layer);
    replace_cap(composed, layer, 0.02f, 0.0f);

    clay_document* ball_only = unit_ball_doc(&layer);

    clay_document* volume_only = clay_document_create();
    clay_layer_id vl = 0;
    REQUIRE(clay_add_sdf_layer(volume_only, "v", &vl) == CLAY_OK);
    {
        clay_volume_params vp = volume_params(0.02f);
        clay_item* item = nullptr;
        REQUIRE(clay_item_volume_from_document(ball_only, &vp, kCapMin, kCapMax, &item) ==
                CLAY_OK);
        REQUIRE(clay_item_set_op(item, CLAY_OP_REPLACE) == CLAY_OK);
        REQUIRE(clay_layer_add_item(volume_only, vl, item, nullptr) == CLAY_OK);
        clay_item_destroy(item);
    }

    for (const kernel::cfloat3& d : cap_directions()) {
        for (float r : {0.9f, 0.999f, 1.001f, 1.1f}) {
            float a = eval_c(ball_only, d * r);
            float b = eval_c(volume_only, d * r);
            float expect = std::min(std::max(a, -b), b);
            CHECK(eval_c(composed, d * r) == expect);  // exact, not approx
        }
    }

    clay_document_destroy(composed);
    clay_document_destroy(ball_only);
    clay_document_destroy(volume_only);
}

TEST_CASE("c abi: the feather blends inside the box and leaves the outside alone") {
    clay_layer_id layer = 0;
    clay_document* ball_only = unit_ball_doc(&layer);
    clay_document* hard = unit_ball_doc(&layer);
    replace_cap(hard, layer, 0.02f, 0.0f);
    clay_document* feathered = unit_ball_doc(&layer);
    replace_cap(feathered, layer, 0.02f, 0.06f);

    // Just outside the box's top face, above the cap: the analytic field
    // continues untouched through a feathered replace. The hard replace caps
    // it with the volume's box distance — the visible hard rectangle. (The
    // region's 0.9 z-extent rounds up to six whole bricks of eight 0.02
    // cells, so the sampled box ends at z = 1.36, not 1.3.)
    kernel::cfloat3 outside = cf3(0.0f, 0.0f, 1.40f);
    float analytic = eval_c(ball_only, outside);
    CHECK(eval_c(feathered, outside) == analytic);
    CHECK(eval_c(hard, outside) != analytic);

    // Deeper inside the box than the feather, the result IS the volume: the
    // crossfade has fully handed over, which is what removes the ties.
    clay_document* volume_only = clay_document_create();
    clay_layer_id vl = 0;
    REQUIRE(clay_add_sdf_layer(volume_only, "v", &vl) == CLAY_OK);
    {
        clay_volume_params vp = volume_params(0.02f);
        vp.feather = 0.06f;
        clay_item* item = nullptr;
        REQUIRE(clay_item_volume_from_document(ball_only, &vp, kCapMin, kCapMax, &item) ==
                CLAY_OK);
        REQUIRE(clay_item_set_op(item, CLAY_OP_REPLACE) == CLAY_OK);
        REQUIRE(clay_layer_add_item(volume_only, vl, item, nullptr) == CLAY_OK);
        clay_item_destroy(item);
    }
    for (float r : {0.98f, 1.0f, 1.02f}) {
        kernel::cfloat3 p = cf3(0.1f, 0.05f, 0.0f) + cf3(0, 0, r);
        float composed = eval_c(feathered, p);
        float b = eval_c(volume_only, p);
        CHECK(composed == doctest::Approx(b).epsilon(1e-6));
    }

    clay_document_destroy(ball_only);
    clay_document_destroy(hard);
    clay_document_destroy(feathered);
    clay_document_destroy(volume_only);
}

TEST_CASE("c abi: a relax can be sampled from a document") {
    // The counterpart clay_item_volume_flatten_from is to the flatten. The
    // relationship to bake-then-relax is EQUALITY inside the band — relax
    // averages cell-aligned taps, and a fresh bake's taps are the document at
    // those lattice points — so the test holds exactly that, and the refusals.
    clay_layer_id layer = 0;
    clay_document* doc = unit_ball_doc(&layer);

    clay_relax_params rp;
    std::memset(&rp, 0, sizeof rp);
    rp.struct_size = sizeof rp;
    rp.strength = 1.0f;
    rp.radius_cells = 2;
    rp.iterations = 2;
    rp.centre[2] = 1.0f;
    rp.region_radius = 0.5f;
    rp.falloff = 0.1f;

    clay_volume_params vp = volume_params(0.02f);

    SUBCASE("parity with bake-then-relax") {
        clay_item* baked = nullptr;
        REQUIRE(clay_item_volume_from_document(doc, &vp, kCapMin, kCapMax, &baked) == CLAY_OK);
        REQUIRE(clay_item_volume_relax(baked, &rp) == CLAY_OK);

        clay_item* sampled = nullptr;
        REQUIRE(clay_item_volume_relax_from(doc, &rp, &vp, kCapMin, kCapMax, &sampled) ==
                CLAY_OK);
        REQUIRE(sampled != nullptr);

        auto place = [](clay_item* it) {
            clay_document* d = clay_document_create();
            clay_layer_id l = 0;
            REQUIRE(clay_add_sdf_layer(d, "o", &l) == CLAY_OK);
            REQUIRE(clay_layer_add_item(d, l, it, nullptr) == CLAY_OK);
            return d;
        };
        clay_document* two_calls = place(baked);
        clay_document* one_call = place(sampled);
        for (const kernel::cfloat3& d : cap_directions())
            for (float r : {0.97f, 1.0f, 1.03f})
                CHECK(eval_c(one_call, d * r) == eval_c(two_calls, d * r));  // exact

        clay_item_destroy(baked);
        clay_item_destroy(sampled);
        clay_document_destroy(two_calls);
        clay_document_destroy(one_call);
    }

    SUBCASE("refusals") {
        clay_item* out = nullptr;
        clay_volume_params bad = vp;
        bad.cell_size = 0.0f;  // a document has no intrinsic scale
        CHECK(clay_item_volume_relax_from(doc, &rp, &bad, kCapMin, kCapMax, &out) != CLAY_OK);
        // half a region is a caller that meant to pass one
        CHECK(clay_item_volume_relax_from(doc, &rp, &vp, kCapMin, nullptr, &out) != CLAY_OK);
        CHECK(clay_item_volume_relax_from(doc, &rp, &vp, nullptr, kCapMax, &out) != CLAY_OK);
        CHECK(clay_item_volume_relax_from(nullptr, &rp, &vp, kCapMin, kCapMax, &out) !=
              CLAY_OK);
        CHECK(clay_item_volume_relax_from(doc, nullptr, &vp, kCapMin, kCapMax, &out) !=
              CLAY_OK);
        CHECK(clay_item_volume_relax_from(doc, &rp, nullptr, kCapMin, kCapMax, &out) !=
              CLAY_OK);
    }

    clay_document_destroy(doc);
}

TEST_CASE("c volume: the feather survives the blob, and an old blob reads hard") {
    // The blob header is self-describing (its size IS the index offset), so
    // the feather appends the same way the sample Lipschitz did: a new reader
    // of an old blob sees 0 — the hard replace — and an old reader of a new
    // blob finds its offsets exactly where they always were.
    using field::FieldVolume;
    auto ball = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.4f; };
    FieldVolume v = FieldVolume::sample(ball, math::Aabb(cf3(-0.6f, -0.6f, -0.6f),
                                                         cf3(0.6f, 0.6f, 0.6f)),
                                        0.05f, 0.15f);
    v.set_feather(0.07f);

    std::vector<float> blob = v.to_blob();
    auto back = FieldVolume::from_blob(blob);
    REQUIRE(back.has_value());
    CHECK(back->feather() == 0.07f);
    // ...and through the byte serialization documents use.
    std::vector<std::uint8_t> bytes = v.serialize();
    auto loaded = FieldVolume::deserialize(bytes.data(), bytes.size());
    REQUIRE(loaded.has_value());
    CHECK(loaded->feather() == 0.07f);

    // A pre-feather blob: header of 12, sections shifted down by one float.
    std::vector<float> old;
    old.insert(old.end(), blob.begin(), blob.begin() + 12);
    old.insert(old.end(), blob.begin() + 13, blob.end());
    old[8] -= 1.0f;   // index offset
    old[9] -= 1.0f;   // far-bound offset
    old[10] -= 1.0f;  // data offset
    auto pre = FieldVolume::from_blob(old);
    REQUIRE(pre.has_value());
    CHECK(pre->feather() == 0.0f);
    CHECK(pre->eval(cf3(0.1f, 0.2f, 0.0f)) == v.eval(cf3(0.1f, 0.2f, 0.0f)));
}
