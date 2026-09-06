// A LAYER's COMPOSITION — the operator it folds into the layers beneath it with
// (fold-the-layers-with-an-operator, scene-model / c-abi / file-io).
//
// THIS FILE COVERS THE MODEL HALF ONLY. At this stage the compiler does not
// read the field yet: every visible SDF layer still hard-unions, so nothing
// here asserts geometry. What it pins is that the value exists, that it is the
// hard union until someone says otherwise, that setting it is one undoable
// step, that a layer which cannot enter the tape refuses it with an error a
// host can read rather than storing state nothing consults, and that it
// survives a save at the current minor while degrading to the union at the
// previous one.
//
// The dangerous direction here is the SERIALIZATION pair. Layer records are not
// length-prefixed, so a writer that emits the block at a minor whose reader
// does not consume it desynchronises every layer record after the first — and
// the reader's bounds checks then reject a document that is not corrupt. Both
// halves or neither, which is what the downgrade case below actually tests.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "clay.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/types.h"
#include "clay/session/sdf_prefix_cache.h"
#include "clay/session/sdf_sculpt.h"

using namespace clay;

namespace {

// Two SDF layers, so a composition on the upper one is the shape the feature is
// for and the lower one is what it would fold onto.
scene::Document two_layers() {
    scene::Document doc;
    for (const char* name : {"base", "cutter"}) {
        scene::Layer& l = doc.add_sdf_layer(name);
        scene::Node n;
        n.prim = scene::Prim::sphere(1.0f);
        l.sdf->insert(n);
    }
    return doc;
}

scene::LayerComposition subtract_smooth() {
    scene::LayerComposition c;
    c.op = scene::Op::Subtract;
    c.blend.profile = scene::BlendProfile::Cubic;
    c.blend.k = 0.25f;
    c.rounding = 0.05f;
    return c;
}

bool same(const scene::LayerComposition& a, const scene::LayerComposition& b) {
    return a.op == b.op && a.blend.profile == b.blend.profile && a.blend.k == b.blend.k &&
           a.rounding == b.rounding;
}

struct CDoc {
    clay_document* d = nullptr;
    clay_layer_id base = 0;
    clay_layer_id cutter = 0;
    CDoc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "base", &base) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "cutter", &cutter) == CLAY_OK);
    }
    ~CDoc() { clay_document_destroy(d); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

}  // namespace

// -- the model ---------------------------------------------------------------

TEST_CASE("a layer's composition defaults to the hard union") {
    // The whole "an existing document is unchanged" claim rests on this one
    // fact: the default IS what every layer has always folded with, so nothing
    // has to be migrated and nothing has to be special-cased.
    scene::Layer fresh;
    CHECK(fresh.composition.op == scene::Op::Add);
    CHECK(fresh.composition.blend.profile == scene::BlendProfile::Hard);
    CHECK(fresh.composition.blend.k == 0.0f);
    CHECK(fresh.composition.rounding == 0.0f);

    scene::Document doc = two_layers();
    for (const scene::Layer& l : doc.layers) CHECK(same(l.composition, scene::LayerComposition{}));
}

TEST_CASE("an instance layer composes independently of the layer it shares with") {
    // The composition lives on the layer RECORD and not on the shared edit
    // list, which is what lets one sculpt appear twice in a stack — once
    // adding, once cutting.
    scene::Document doc = two_layers();
    const scene::LayerId src = doc.layers[0].id;
    scene::Layer* inst = doc.instance_layer(src, "copia");
    REQUIRE(inst != nullptr);
    const scene::LayerId inst_id = inst->id;
    CHECK((doc.find_layer(src)->sdf.get() == doc.find_layer(inst_id)->sdf.get()));

    REQUIRE(scene::apply(doc, scene::SetLayerCompositionCmd{inst_id, subtract_smooth()})
                .has_value());
    CHECK(doc.find_layer(inst_id)->composition.op == scene::Op::Subtract);
    CHECK(doc.find_layer(src)->composition.op == scene::Op::Add);
}

// -- the command -------------------------------------------------------------

TEST_CASE("setting a composition is one undoable step") {
    scene::Document doc = two_layers();
    const scene::LayerId id = doc.layers[1].id;
    const std::vector<std::uint8_t> before = scene::serialize_document(doc);

    scene::UndoStack undo;
    REQUIRE(undo.perform(doc, scene::SetLayerCompositionCmd{id, subtract_smooth()}));
    CHECK(scene::serialize_document(doc) != before);  // it did something
    CHECK(same(doc.find_layer(id)->composition, subtract_smooth()));

    REQUIRE(undo.undo(doc));
    CHECK(scene::serialize_document(doc) == before);  // bit-identical restore
    CHECK(same(doc.find_layer(id)->composition, scene::LayerComposition{}));

    REQUIRE(undo.redo(doc));
    CHECK(same(doc.find_layer(id)->composition, subtract_smooth()));
}

TEST_CASE("a composition command serializes and deserializes losslessly") {
    const scene::Command cmd = scene::SetLayerCompositionCmd{7, subtract_smooth()};
    const std::vector<std::uint8_t> bytes = scene::serialize(cmd);
    const std::optional<scene::Command> back = scene::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    REQUIRE(back->index() == cmd.index());
    const auto& got = std::get<scene::SetLayerCompositionCmd>(*back);
    CHECK(got.id == 7);
    CHECK(same(got.composition, subtract_smooth()));
    CHECK(scene::serialize(*back) == bytes);
}

TEST_CASE("a protected layer refuses a composition") {
    scene::Document doc = two_layers();
    const scene::LayerId id = doc.layers[1].id;
    doc.find_layer(id)->locked = true;
    const scene::Command cmd = scene::SetLayerCompositionCmd{id, subtract_smooth()};
    CHECK_FALSE(scene::apply(doc, cmd).has_value());
    CHECK(same(doc.find_layer(id)->composition, scene::LayerComposition{}));
}

TEST_CASE("a non-SDF layer refuses a composition rather than storing it") {
    // Refused in the VOCABULARY and not only at the binding, so a replayed
    // journal cannot install what the setter rejects.
    scene::Document doc = two_layers();
    scene::Layer mesh;
    mesh.id = doc.reserve_layer_id();
    mesh.name = "imported";
    mesh.kind = scene::LayerKind::Mesh;
    const scene::LayerId mesh_id = mesh.id;
    doc.layers.push_back(std::move(mesh));

    const std::vector<std::uint8_t> before = scene::serialize_document(doc);
    CHECK_FALSE(
        scene::apply(doc, scene::SetLayerCompositionCmd{mesh_id, subtract_smooth()}).has_value());
    CHECK(scene::serialize_document(doc) == before);
    CHECK(same(doc.find_layer(mesh_id)->composition, scene::LayerComposition{}));
}

// -- invalidation ------------------------------------------------------------

TEST_CASE("a layer's digest moves for a composition change") {
    // Both consumers of `mix_layer_head` at once: the whole-layer fingerprint a
    // Smooth transaction checks, and the prefix fingerprint the SDF prefix
    // cache checks. A field neither of them sees is a cache serving a field
    // compiled under a different fold, with no error to say so — and the prefix
    // cache's own header calls its fingerprint check THE SAFETY NET, because
    // invalidation by command is an optimisation and can be forgotten.
    scene::Document doc = two_layers();
    const scene::Layer& base = doc.layers[1];
    const std::uint64_t before = session::layer_fingerprint(base);
    const std::size_t roots = base.sdf->roots.size();
    const std::uint64_t prefix_before = session::layer_prefix_fingerprint(base, roots);

    scene::Layer composed = base;
    composed.composition = subtract_smooth();
    CHECK(session::layer_fingerprint(composed) != before);
    CHECK(session::layer_prefix_fingerprint(composed, roots) != prefix_before);

    // Each field on its own, so a digest that mixed only the op still fails.
    for (scene::LayerComposition c :
         {scene::LayerComposition{scene::Op::Subtract, scene::Blend{}, 0.0f},
          scene::LayerComposition{scene::Op::Add, scene::Blend{scene::BlendProfile::Cubic, 0.0f},
                                  0.0f},
          scene::LayerComposition{scene::Op::Add, scene::Blend{scene::BlendProfile::Hard, 0.2f},
                                  0.0f},
          scene::LayerComposition{scene::Op::Add, scene::Blend{}, 0.1f}}) {
        scene::Layer one = base;
        one.composition = c;
        CHECK(session::layer_fingerprint(one) != before);
    }
}

// -- serialization -----------------------------------------------------------

TEST_CASE("a layer's composition round-trips at the current minor") {
    scene::Document doc = two_layers();
    doc.layers[1].composition = subtract_smooth();
    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc, scene::kSceneMinor);
    const std::optional<scene::Document> back =
        scene::deserialize_document(bytes.data(), bytes.size(), scene::kSceneMinor);
    REQUIRE(back.has_value());
    REQUIRE(back->layers.size() == 2);
    CHECK(same(back->layers[0].composition, scene::LayerComposition{}));
    CHECK(same(back->layers[1].composition, subtract_smooth()));
    // And re-writing it reproduces the stream, so the reader consumed exactly
    // what the writer emitted rather than stopping short and getting away with
    // it on the last record.
    CHECK(scene::serialize_document(*back, scene::kSceneMinor) == bytes);
}

TEST_CASE("writing below minor 18 refuses a document that carries a composition") {
    // THE DEPARTURE, and the case that makes it. Every earlier minor is
    // writable at the previous one by degrading — an unsquashed layer, an
    // instance that comes back a copy — and none of those is a different
    // sculpture. A subtractive layer written at 17 comes back UNIONING: the
    // cutter that was carving a hole is a lump welded onto the form, in a file
    // that opens cleanly and looks deliberate. So the write is refused.
    scene::Document doc = two_layers();
    doc.layers[1].composition = subtract_smooth();
    const scene::LayerId cutter = doc.layers[1].id;

    CHECK(scene::layer_blocking_minor(doc, 17) == cutter);
    CHECK(scene::layer_blocking_minor(doc, 12) == cutter);  // and every minor below
    CHECK(scene::layer_blocking_minor(doc, scene::kSceneMinor) == 0);
    // An empty vector, which is never a valid stream: even a document with no
    // layers writes its layer count.
    CHECK(scene::serialize_document(doc, 17).empty());
    CHECK_FALSE(scene::serialize_document(doc, scene::kSceneMinor).empty());

    SUBCASE("a blend, a radius or a rounding alone is enough to block it") {
        using LC = scene::LayerComposition;
        using B = scene::Blend;
        for (LC c : {LC{scene::Op::Add, B{scene::BlendProfile::Cubic, 0.0f}, 0.0f},
                     LC{scene::Op::Add, B{scene::BlendProfile::Hard, 0.2f}, 0.0f},
                     LC{scene::Op::Add, B{}, 0.1f}}) {
            scene::Document d = two_layers();
            d.layers[1].composition = c;
            CHECK(scene::layer_blocking_minor(d, 17) == d.layers[1].id);
        }
    }
    SUBCASE("a non-SDF layer never blocks it — it carries no composition at all") {
        scene::Document d = two_layers();
        scene::Layer mesh;
        mesh.id = d.reserve_layer_id();
        mesh.kind = scene::LayerKind::Mesh;
        mesh.composition = subtract_smooth();  // dead state; nothing can set it
        d.layers.push_back(std::move(mesh));
        CHECK(scene::layer_blocking_minor(d, 17) == 0);
    }
}

TEST_CASE("a document where every layer unions still writes at minor 17, byte for byte") {
    // The other half of the rule: the refusal covers exactly the documents 17
    // cannot express, and every document it CAN express is written exactly as
    // 17 always wrote it. Otherwise "refuse" would have quietly become "never
    // write an older minor again".
    scene::Document doc = two_layers();
    CHECK(scene::layer_blocking_minor(doc, 17) == 0);
    const std::vector<std::uint8_t> old_bytes = scene::serialize_document(doc, 17);
    REQUIRE_FALSE(old_bytes.empty());

    // The bytes a build that predates the field would have written, exactly: at
    // 17 the ten-byte block is not there, so the record is what it always was.
    // A one-sided gate — a writer that emits it at 17, or a reader that consumes
    // it at 17 — fails here, and it has to, because layer records carry no
    // length and every record after the first would desynchronise.
    const std::vector<std::uint8_t> now = scene::serialize_document(doc, scene::kSceneMinor);
    CHECK(now.size() == old_bytes.size() + 10 * doc.layers.size());
    CHECK(now != old_bytes);

    const std::optional<scene::Document> back =
        scene::deserialize_document(old_bytes.data(), old_bytes.size(), 17);
    REQUIRE(back.has_value());
    REQUIRE(back->layers.size() == 2);
    for (const scene::Layer& l : back->layers)
        CHECK(same(l.composition, scene::LayerComposition{}));
}

// -- the C ABI ---------------------------------------------------------------

TEST_CASE("c abi: a layer's composition is set and read back") {
    CDoc doc;
    int32_t op = -1, blend = -1;
    float k = -1.0f, rounding = -1.0f;
    REQUIRE(clay_document_layer_composition(doc.d, doc.cutter, &op, &blend, &k, &rounding) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_ADD);
    CHECK(blend == CLAY_BLEND_HARD);
    CHECK(k == 0.0f);
    CHECK(rounding == 0.0f);

    REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, CLAY_OP_SUBTRACT,
                                                CLAY_BLEND_CUBIC, 0.25f, 0.05f) == CLAY_OK);
    REQUIRE(clay_document_layer_composition(doc.d, doc.cutter, &op, &blend, &k, &rounding) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_SUBTRACT);
    CHECK(blend == CLAY_BLEND_CUBIC);
    CHECK(k == doctest::Approx(0.25f));
    CHECK(rounding == doctest::Approx(0.05f));

    // What comes out goes straight back in.
    CHECK(clay_document_set_layer_composition(doc.d, doc.cutter, op, blend, k, rounding) ==
          CLAY_OK);
    // Every out-pointer is optional; the call still validates the layer.
    CHECK(clay_document_layer_composition(doc.d, doc.cutter, nullptr, nullptr, nullptr,
                                          nullptr) == CLAY_OK);
    // And the other layer was not touched.
    REQUIRE(clay_document_layer_composition(doc.d, doc.base, &op, nullptr, nullptr, nullptr) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_ADD);
}

TEST_CASE("c abi: what a layer composition refuses") {
    CDoc doc;
    const clay_layer_id l = doc.cutter;

    SUBCASE("an unknown op, and the group's inline op") {
        CHECK(clay_document_set_layer_composition(doc.d, l, 99, CLAY_BLEND_HARD, 0, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_INLINE, CLAY_BLEND_HARD, 0,
                                                  0) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a transition, which has no node to read its parameters from") {
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_TRANSITION_LINEAR,
                                                  CLAY_BLEND_HARD, 0, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_TRANSITION_RADIAL,
                                                  CLAY_BLEND_HARD, 0, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("an unknown blend profile, and negative or non-finite floats") {
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_ADD, 99, 0, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_ADD, CLAY_BLEND_CUBIC, -0.1f,
                                                  0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_ADD, CLAY_BLEND_CUBIC, 0,
                                                  -0.1f) == CLAY_ERROR_INVALID_ARGUMENT);
        const float inf = std::numeric_limits<float>::infinity();
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_ADD, CLAY_BLEND_CUBIC, inf,
                                                  0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_ADD, CLAY_BLEND_CUBIC, 0,
                                                  std::nanf("")) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a layer that is not there") {
        CHECK(clay_document_set_layer_composition(doc.d, 9999, CLAY_OP_SUBTRACT, CLAY_BLEND_HARD,
                                                  0, 0) == CLAY_ERROR_NOT_FOUND);
        CHECK(clay_document_layer_composition(doc.d, 9999, nullptr, nullptr, nullptr, nullptr) ==
              CLAY_ERROR_NOT_FOUND);
        CHECK(clay_document_layer_composition(nullptr, l, nullptr, nullptr, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a protected layer, which is refused as every other edit is") {
        REQUIRE(clay_document_set_layer_protection(doc.d, l, 0, 1) == CLAY_OK);
        CHECK(clay_document_set_layer_composition(doc.d, l, CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0,
                                                  0) == CLAY_ERROR_INVALID_ARGUMENT);
        // Reading is not editing.
        int32_t op = -1;
        REQUIRE(clay_document_layer_composition(doc.d, l, &op, nullptr, nullptr, nullptr) ==
                CLAY_OK);
        CHECK(op == CLAY_OP_ADD);
    }

    // Nothing above stored anything.
    int32_t op = -1;
    REQUIRE(clay_document_layer_composition(doc.d, l, &op, nullptr, nullptr, nullptr) == CLAY_OK);
    CHECK(op == CLAY_OP_ADD);
}

TEST_CASE("c abi: a voxel layer refuses a composition at both ends") {
    CDoc doc;
    clay_layer_id voxel = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc.d, "grelha", 0.05f, &voxel, &grid) == CLAY_OK);

    CHECK(clay_document_set_layer_composition(doc.d, voxel, CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0,
                                              0) == CLAY_ERROR_INVALID_ARGUMENT);
    // The reader refuses rather than answering CLAY_OP_ADD, which would read as
    // a valid hard union on a layer that cannot carry one.
    int32_t op = -1;
    CHECK(clay_document_layer_composition(doc.d, voxel, &op, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(op == -1);  // and wrote nothing
}

TEST_CASE("c abi: a composition survives a save and a load, and is undoable") {
    CDoc doc;
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, CLAY_OP_INTERSECT,
                                                CLAY_BLEND_QUADRATIC, 0.3f, 0.02f) == CLAY_OK);

    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc.d, &blob) == CLAY_OK);
    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob), &back) ==
            CLAY_OK);
    int32_t op = -1, blend = -1;
    float k = -1.0f, rounding = -1.0f;
    REQUIRE(clay_document_layer_composition(back, doc.cutter, &op, &blend, &k, &rounding) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_INTERSECT);
    CHECK(blend == CLAY_BLEND_QUADRATIC);
    CHECK(k == doctest::Approx(0.3f));
    CHECK(rounding == doctest::Approx(0.02f));
    clay_document_destroy(back);
    clay_blob_destroy(blob);

    // One undo step through the ordinary layer-property history.
    int32_t stepped = 0;
    REQUIRE(clay_document_undo(doc.d, &stepped) == CLAY_OK);
    CHECK(stepped == 1);
    REQUIRE(clay_document_layer_composition(doc.d, doc.cutter, &op, nullptr, nullptr, nullptr) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_ADD);
    REQUIRE(clay_document_redo(doc.d, &stepped) == CLAY_OK);
    CHECK(stepped == 1);
    REQUIRE(clay_document_layer_composition(doc.d, doc.cutter, &op, nullptr, nullptr, nullptr) ==
            CLAY_OK);
    CHECK(op == CLAY_OP_INTERSECT);
}

TEST_CASE("c abi: setting a composition changes nothing about the field yet") {
    // The honest statement of what this stage landed: the model carries the
    // value and the compiler does not read it. When the fold lands this case
    // becomes false and must be REPLACED by the parity and order gates, not
    // deleted quietly.
    CDoc doc;
    clay_item_desc sphere;
    std::memset(&sphere, 0, sizeof sphere);
    sphere.struct_size = static_cast<uint32_t>(sizeof sphere);
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 1.0f;
    sphere.rotation[3] = 1.0f;
    sphere.scale = 1.0f;
    REQUIRE(clay_add_item(doc.d, doc.base, &sphere, nullptr) == CLAY_OK);
    sphere.position[0] = 0.5f;
    sphere.params[0] = 0.6f;
    REQUIRE(clay_add_item(doc.d, doc.cutter, &sphere, nullptr) == CLAY_OK);

    const float pts[9] = {0.0f, 0.0f, 0.0f, 0.7f, 0.0f, 0.0f, 1.2f, 0.3f, 0.0f};
    float before[3] = {0, 0, 0};
    REQUIRE(clay_eval_points(doc.d, nullptr, pts, 3, before, nullptr) == CLAY_OK);
    REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, CLAY_OP_SUBTRACT,
                                                CLAY_BLEND_HARD, 0, 0) == CLAY_OK);
    float after[3] = {0, 0, 0};
    REQUIRE(clay_eval_points(doc.d, nullptr, pts, 3, after, nullptr) == CLAY_OK);
    for (int i = 0; i < 3; ++i) CHECK(after[i] == before[i]);  // bit-identical
}

TEST_CASE("c abi: a host can ask whether an older format can still say this") {
    CDoc doc;
    clay_layer_id blocking = 99;
    REQUIRE(clay_document_writable_at_minor(doc.d, 17, &blocking) == CLAY_OK);
    CHECK(blocking == 0);

    REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, CLAY_OP_SUBTRACT,
                                                CLAY_BLEND_HARD, 0, 0) == CLAY_OK);
    CHECK(clay_document_writable_at_minor(doc.d, 17, &blocking) == CLAY_ERROR_UNSUPPORTED);
    CHECK(blocking == doc.cutter);  // and it NAMES the layer, so a host can say which
    CHECK(std::strlen(clay_last_error()) > 0);

    // The question is only ever about writing DOWN: this build's own layout and
    // anything beyond it lose nothing.
    blocking = 99;
    CHECK(clay_document_writable_at_minor(doc.d, 18, &blocking) == CLAY_OK);
    CHECK(blocking == 0);
    CHECK(clay_document_writable_at_minor(doc.d, 999, nullptr) == CLAY_OK);

    CHECK(clay_document_writable_at_minor(doc.d, 0, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_writable_at_minor(nullptr, 17, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    // Setting it back to the union makes the document writable again — the
    // refusal is a property of the document, not a latch.
    REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, CLAY_OP_ADD, CLAY_BLEND_HARD, 0,
                                                0) == CLAY_OK);
    CHECK(clay_document_writable_at_minor(doc.d, 17, nullptr) == CLAY_OK);
}
