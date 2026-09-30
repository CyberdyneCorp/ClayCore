#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "clay.h"
#include "clay/brush/move.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

// Per-item mirror axes (issue #664). An item's mirror participation was a bool
// and the axes lived on the layer, so a host that turned symmetry off, or
// switched it from X to Y, changed every item made under the old axes with it:
// a lump sculpted on +x under X symmetry lost its -x twin. An item can now
// carry its OWN axes, which replace the layer's for it; the layer's stay the
// default for every item that sets none, which is every item in every document
// saved before.
//
// The probe throughout: a lone lump of radius 0.25 at (0.6, 0.4, 0), and the
// field at its centre's reflections. A copy there reads about -0.25; no copy
// reads the distance to the lump itself, far positive.

using namespace clay;

namespace {

constexpr float kLump[3] = {0.6f, 0.4f, 0.0f};
constexpr float kR = 0.25f;

// ---- scene level -------------------------------------------------------------

scene::Document lump_doc(std::uint8_t layer_axes, std::uint8_t own_axes, bool mirror = true) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    l.mirror_axes = layer_axes;
    scene::Node n;
    n.prim = scene::Prim::sphere(kR);
    n.xform.position = kernel::cf3(kLump[0], kLump[1], kLump[2]);
    n.mirror = mirror;
    n.own_mirror_axes = own_axes;
    l.sdf->insert(n);
    return doc;
}

float field_at(const scene::Document& doc, float x, float y, float z) {
    return scene::compile_document(doc).eval(kernel::cf3(x, y, z)).d;
}

bool twin_x(const scene::Document& doc) { return field_at(doc, -kLump[0], kLump[1], 0) < 0.0f; }
bool twin_y(const scene::Document& doc) { return field_at(doc, kLump[0], -kLump[1], 0) < 0.0f; }

scene::NodeId only_node(const scene::Document& doc) {
    return doc.layers.front().sdf->roots.front();
}

// ---- C ABI -------------------------------------------------------------------

struct CDoc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    CDoc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
    }
    ~CDoc() { clay_document_destroy(d); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

clay_node_id add_lump(CDoc& doc, int own_axes /* -1: never set */) {
    const float params[1] = {kR};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_position(item, kLump) == CLAY_OK);
    if (own_axes >= 0)
        REQUIRE(clay_item_set_mirror_axes(item, static_cast<uint8_t>(own_axes)) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);
    return node;
}

float c_field(const clay_document* doc, float x, float y, float z) {
    const float p[3] = {x, y, z};
    float d = 0.0f;
    REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
    return d;
}

bool c_twin_x(const clay_document* doc) { return c_field(doc, -kLump[0], kLump[1], 0) < 0.0f; }
bool c_twin_y(const clay_document* doc) { return c_field(doc, kLump[0], -kLump[1], 0) < 0.0f; }

void set_layer_mirror(CDoc& doc, int x, int y, int z) {
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, x, y, z, 0.0f) == CLAY_OK);
}

}  // namespace

// ---- old documents -----------------------------------------------------------

TEST_CASE("a document saved before per-item axes loads inheriting and evaluates as saved") {
    // Written at minor 19 — the layout every earlier build writes — with a
    // participating item and an opted-out one under a layer mirror X.
    scene::Document doc = lump_doc(scene::kMirrorX, scene::kMirrorAxesInherit);
    {
        scene::Node out;
        out.prim = scene::Prim::sphere(kR);
        out.xform.position = kernel::cf3(0.6f, -0.9f, 0.0f);
        out.mirror = false;
        doc.layers.front().sdf->insert(out);
    }
    const std::vector<std::uint8_t> old = scene::serialize_document(doc, 19);
    REQUIRE_FALSE(old.empty());
    const std::optional<scene::Document> back =
        scene::deserialize_document(old.data(), old.size(), 19);
    REQUIRE(back.has_value());
    for (const auto& [id, n] : back->layers.front().sdf->nodes()) {
        (void)id;
        CHECK(n.own_mirror_axes == scene::kMirrorAxesInherit);
    }
    // Same field, sample for sample, as the document that was saved.
    for (float x : {-0.6f, 0.6f, 0.0f})
        for (float y : {-0.9f, -0.4f, 0.4f})
            CHECK(field_at(*back, x, y, 0.0f) == field_at(doc, x, y, 0.0f));
    CHECK(twin_x(*back));
    CHECK(field_at(*back, -0.6f, -0.9f, 0.0f) > 0.0f);  // the opted-out one stays single

    SUBCASE("and a document whose items all inherit still writes the bytes 19 wrote") {
        CHECK(scene::layer_blocking_minor(*back, 19) == 0);
        CHECK(scene::serialize_document(*back, 19) == old);
    }
}

TEST_CASE("an item's own axes round-trip at minor 20 and block every minor below") {
    scene::Document doc = lump_doc(0, scene::kMirrorX);
    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc, scene::kSceneMinor);
    const std::optional<scene::Document> back =
        scene::deserialize_document(bytes.data(), bytes.size(), scene::kSceneMinor);
    REQUIRE(back.has_value());
    CHECK(back->layers.front().sdf->find(only_node(*back))->own_mirror_axes == scene::kMirrorX);
    CHECK(twin_x(*back));
    CHECK(scene::serialize_document(*back, scene::kSceneMinor) == bytes);

    // Dropping the byte would hand the item the layer's (absent) mirror and
    // lose its twin in a file that opens cleanly, so the write is refused.
    const scene::LayerId layer = doc.layers.front().id;
    CHECK(scene::layer_blocking_minor(doc, 19) == layer);
    CHECK(scene::layer_blocking_minor(doc, 14) == layer);
    CHECK(scene::layer_blocking_minor(doc, scene::kSceneMinor) == 0);
    CHECK(scene::serialize_document(doc, 19).empty());

    SUBCASE("own axes of 0 block too: 19 would give the item the layer's twin") {
        scene::Document zero = lump_doc(scene::kMirrorX, 0);
        CHECK_FALSE(twin_x(zero));
        CHECK(scene::layer_blocking_minor(zero, 19) == zero.layers.front().id);
    }
}

// ---- evaluation --------------------------------------------------------------

TEST_CASE("an item's own axes override the layer's mirror") {
    CHECK(twin_x(lump_doc(scene::kMirrorX, scene::kMirrorAxesInherit)));  // the default
    CHECK_FALSE(twin_y(lump_doc(scene::kMirrorX, scene::kMirrorAxesInherit)));

    const scene::Document own_y = lump_doc(scene::kMirrorX, scene::kMirrorY);
    CHECK_FALSE(twin_x(own_y));
    CHECK(twin_y(own_y));

    const scene::Document own_none = lump_doc(scene::kMirrorX, 0);
    CHECK_FALSE(twin_x(own_none));
    CHECK_FALSE(twin_y(own_none));

    // Own axes win over the participation flag too: the flag governs only
    // what an INHERITING item takes from the layer.
    CHECK(twin_x(lump_doc(0, scene::kMirrorX, /*mirror=*/false)));
}

TEST_CASE("switching the layer mirror leaves an item with its own axes as it was made") {
    CDoc doc;
    set_layer_mirror(doc, 1, 0, 0);
    const clay_node_id made_under_x = add_lump(doc, CLAY_MIRROR_X);
    REQUIRE(c_twin_x(doc.d));

    // The host turns symmetry OFF: the layer's default goes, the item's twin
    // stays.
    set_layer_mirror(doc, 0, 0, 0);
    CHECK(c_twin_x(doc.d));
    CHECK_FALSE(c_twin_y(doc.d));

    // ...and points it at Y: an inheriting item made now takes Y, the old one
    // keeps X and gains nothing.
    set_layer_mirror(doc, 0, 1, 0);
    CHECK(c_twin_x(doc.d));
    CHECK_FALSE(c_twin_y(doc.d));
    const clay_node_id inheriting = add_lump(doc, -1);
    (void)inheriting;
    CHECK(c_twin_y(doc.d));

    int32_t mirror = 0;
    uint8_t axes = 0, effective = 0;
    REQUIRE(clay_layer_node_mirror(doc.d, doc.layer, made_under_x, &mirror, &axes, &effective) ==
            CLAY_OK);
    CHECK(mirror == 1);
    CHECK(axes == CLAY_MIRROR_X);
    CHECK(effective == CLAY_MIRROR_X);
    REQUIRE(clay_layer_node_mirror(doc.d, doc.layer, inheriting, &mirror, &axes, &effective) ==
            CLAY_OK);
    CHECK(axes == CLAY_MIRROR_AXES_INHERIT);
    CHECK(effective == CLAY_MIRROR_Y);
}

TEST_CASE("the brick cache sees an item's own twin where the layer has no mirror") {
    // The cull reads the item's bound: a bound built from the LAYER's axes
    // alone would stop at the +x lump and a brick holding only the twin would
    // compile without it. Compared against the full document tape.
    CDoc doc;
    add_lump(doc, CLAY_MIRROR_X);
    clay_brick_config cfg;
    cfg.struct_size = sizeof(cfg);
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);
    const float lo[3] = {-1.5f, -1.5f, -1.5f}, hi[3] = {1.5f, 1.5f, 1.5f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    const std::size_t samples = static_cast<std::size_t>(cfg.dim) * cfg.dim * cfg.dim;
    std::vector<clay_brick_request> reqs(64);
    std::vector<float> values(64 * samples);
    std::vector<int32_t> results(64);
    for (;;) {
        std::size_t count = reqs.size(), remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, reqs.data(), count, values.data(),
                                               count * samples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(),
                                        count * samples, nullptr, 0, results.data(),
                                        &accepted) == CLAY_OK);
        if (remaining == 0) break;
    }
    // A ray along -x at the lump's height, from beyond the twin.
    const float origin[3] = {-2.0f, kLump[1], 0.0f};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    int32_t hit = 0;
    float t = 0.0f;
    REQUIRE(clay_brick_cache_raycast(cache, origin, dir, &hit, &t, nullptr, nullptr) == CLAY_OK);
    CHECK(hit == 1);
    CHECK(t == doctest::Approx(2.0f - kLump[0] - kR).epsilon(0.02));
    clay_brick_cache_destroy(cache);
}

// ---- the placed-node setter --------------------------------------------------

TEST_CASE("a placed item's mirror is one undoable edit") {
    CDoc doc;
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    set_layer_mirror(doc, 1, 0, 0);
    const clay_node_id node = add_lump(doc, -1);  // saved with the default: follows X
    REQUIRE(c_twin_x(doc.d));

    // Correct it in place: own axes Y, participation off (radial only).
    REQUIRE(clay_layer_set_node_mirror(doc.d, doc.layer, node, -1, CLAY_MIRROR_Y) == CLAY_OK);
    int32_t mirror = 0;
    uint8_t axes = 0, effective = 0;
    REQUIRE(clay_layer_node_mirror(doc.d, doc.layer, node, &mirror, &axes, &effective) == CLAY_OK);
    CHECK(mirror == -1);
    CHECK(axes == CLAY_MIRROR_Y);
    CHECK(effective == CLAY_MIRROR_Y);
    CHECK_FALSE(c_twin_x(doc.d));
    CHECK(c_twin_y(doc.d));

    // One undo puts back BOTH values.
    int32_t undone = 0;
    REQUIRE(clay_document_undo(doc.d, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_layer_node_mirror(doc.d, doc.layer, node, &mirror, &axes, &effective) == CLAY_OK);
    CHECK(mirror == 1);
    CHECK(axes == CLAY_MIRROR_AXES_INHERIT);
    CHECK(effective == CLAY_MIRROR_X);
    CHECK(c_twin_x(doc.d));
    CHECK_FALSE(c_twin_y(doc.d));

    int32_t redone = 0;
    REQUIRE(clay_document_redo(doc.d, &redone) == CLAY_OK);
    CHECK(redone == 1);
    REQUIRE(clay_layer_node_mirror(doc.d, doc.layer, node, &mirror, &axes, nullptr) == CLAY_OK);
    CHECK(mirror == -1);
    CHECK(axes == CLAY_MIRROR_Y);
    CHECK(c_twin_y(doc.d));

    SUBCASE("inherit puts the item back on the layer") {
        REQUIRE(clay_layer_set_node_mirror(doc.d, doc.layer, node, 1, CLAY_MIRROR_AXES_INHERIT) ==
                CLAY_OK);
        CHECK(c_twin_x(doc.d));
        CHECK_FALSE(c_twin_y(doc.d));
    }
}

TEST_CASE("the mirror-axes entry points refuse what they cannot mean") {
    CDoc doc;
    const clay_node_id node = add_lump(doc, -1);
    CHECK(clay_layer_set_node_mirror(doc.d, doc.layer, node, 1, 8) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_node_mirror(doc.d, doc.layer, 999, 1, 0) == CLAY_ERROR_NOT_FOUND);
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f,
                                 &group) == CLAY_OK);
    CHECK(clay_layer_set_node_mirror(doc.d, doc.layer, group, 1, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_node_mirror(doc.d, doc.layer, group, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    const float params[1] = {kR};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
    REQUIRE(item != nullptr);
    uint8_t axes = 0;
    REQUIRE(clay_item_mirror_axes(item, &axes) == CLAY_OK);
    CHECK(axes == CLAY_MIRROR_AXES_INHERIT);
    CHECK(clay_item_set_mirror_axes(item, 0x10) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_mirror_axes(item, CLAY_MIRROR_X | CLAY_MIRROR_Z) == CLAY_OK);
    REQUIRE(clay_item_mirror_axes(item, &axes) == CLAY_OK);
    CHECK(axes == (CLAY_MIRROR_X | CLAY_MIRROR_Z));
    clay_item_destroy(item);
}

TEST_CASE("saving at an older minor names the layer an item's own axes block") {
    CDoc doc;
    add_lump(doc, CLAY_MIRROR_X);
    clay_layer_id blocking = 0;
    CHECK(clay_document_writable_at_minor(doc.d, 19, &blocking) == CLAY_ERROR_UNSUPPORTED);
    CHECK(blocking == doc.layer);
    clay_blob* blob = nullptr;
    CHECK(clay_document_save_memory_at_minor(doc.d, 19, &blob, &blocking) ==
          CLAY_ERROR_UNSUPPORTED);
    CHECK(blob == nullptr);

    // At the current minor it saves, and loads with the axes and the twin.
    REQUIRE(clay_document_save_memory(doc.d, &blob) == CLAY_OK);
    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob), &back) ==
            CLAY_OK);
    clay_blob_destroy(blob);
    CHECK(c_twin_x(back));
    clay_document_destroy(back);
}

// ---- the Move brush ------------------------------------------------------------

TEST_CASE("a drag reaches an item through the item's own reflections") {
    // The intended behaviour, stated in brush/move.h: the images of a drag are
    // the copies the compiler emits of THIS item. Dragging at the -x twin of an
    // item that kept X on a layer with no mirror moves it (both sides are the
    // item); dragging at the layer's reflection of an item held at 0 does not.
    const brush::MoveSettings settings{0.2f, 0, false, 0};
    const kernel::cfloat3 at_twin = kernel::cf3(-kLump[0], kLump[1] + kR, 0.0f);

    SUBCASE("own X on an unmirrored layer: the twin is reachable") {
        const scene::Document doc = lump_doc(0, scene::kMirrorX);
        const std::vector<brush::PreparedMove> prepared =
            brush::prepare_move(doc.layers.front(), at_twin, settings);
        REQUIRE(prepared.size() == 1);
        CHECK(prepared.front().images.size() == 2);
        CHECK(prepared.front().own_mirror_axes == scene::kMirrorX);
        // And the reach the host is told covers the twin's ball.
        CHECK(brush::drag_images(doc.layers.front(), at_twin, kernel::cf3(0, 0.1f, 0),
                                 brush::prepared_own_mirror_axes(prepared))
                  .size() == 2);
    }
    SUBCASE("own 0 on a mirrored layer: only the touched side moves") {
        const scene::Document doc = lump_doc(scene::kMirrorX, 0);
        CHECK(brush::prepare_move(doc.layers.front(), at_twin, settings).empty());
        const kernel::cfloat3 on_item = kernel::cf3(kLump[0], kLump[1] + kR, 0.0f);
        const std::vector<brush::PreparedMove> prepared =
            brush::prepare_move(doc.layers.front(), on_item, settings);
        REQUIRE(prepared.size() == 1);
        CHECK(prepared.front().images.size() == 1);
    }
    SUBCASE("an inheriting item still sees the layer's images, exactly as before") {
        const scene::Document doc = lump_doc(scene::kMirrorX, scene::kMirrorAxesInherit);
        const std::vector<brush::PreparedMove> prepared =
            brush::prepare_move(doc.layers.front(), at_twin, settings);
        REQUIRE(prepared.size() == 1);
        CHECK(prepared.front().images.size() == 2);
        CHECK(prepared.front().own_mirror_axes == 0);
    }
}

TEST_CASE("the chain pad counts an item's own copies") {
    // An item's own axes lengthen the layer's chain exactly as the layer's
    // would, so the multiplicity the envelope resolves against includes them.
    const scene::Document doc = lump_doc(0, scene::kMirrorX | scene::kMirrorY);
    const scene::Layer& l = doc.layers.front();
    const scene::CullPadTerms terms = scene::cull_pad_terms(*l.sdf, l);
    CHECK(terms.own_mirror_axes == (scene::kMirrorX | scene::kMirrorY));
    CHECK(scene::layer_symmetry_multiplicity(l, terms.own_mirror_axes) == 3);
    CHECK(scene::layer_symmetry_multiplicity(l) == 1);
}
