// Ghosted and locked layers (scene-model + picking specs,
// add-layer-ghost-lock): the flags, their effect on edits and picking, and
// the things they deliberately do NOT change.

#include <doctest/doctest.h>

#include "clay.h"
#include "clay/io/clayspace.h"
#include "clay/pick/pick.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;

namespace {

// Takes an id rather than a reference: adding a layer can reallocate the
// document's layer vector, so a reference held across a second add_sdf_layer
// dangles.
scene::NodeId add_sphere(scene::Document& doc, scene::LayerId layer, float radius,
                         kernel::cfloat3 at) {
    scene::Node n;
    n.prim = scene::Prim::sphere(radius);
    n.xform.position = at;
    return doc.find_layer(layer)->sdf->insert(std::move(n));
}

}  // namespace

TEST_CASE("protection: neither flag changes the field") {
    scene::Document doc;
    const scene::LayerId layer = doc.add_sdf_layer("body").id;
    add_sphere(doc, layer, 1.0f, cf3(0, 0, 0));

    std::vector<kernel::cfloat3> probes;
    for (float x = -2.0f; x <= 2.0f; x += 0.29f)
        for (float y = -2.0f; y <= 2.0f; y += 0.31f) probes.push_back(cf3(x, y, 0.13f));

    auto evaluate = [&]() {
        scene::Tape t = scene::compile_document(doc);
        std::vector<float> out;
        for (kernel::cfloat3 p : probes) out.push_back(t.eval(p).d);
        return out;
    };
    std::vector<float> before = evaluate();

    scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, true, true}});
    CHECK(doc.find_layer(layer)->ghost);
    CHECK(doc.find_layer(layer)->locked);
    CHECK(evaluate() == before);  // bit-identical, not merely close
}

TEST_CASE("protection: setting it is undoable and round trips") {
    io::ClaySpaceDoc file;
    const scene::LayerId gid = file.document.add_sdf_layer("reference").id;
    const scene::LayerId lid = file.document.add_sdf_layer("finished").id;
    add_sphere(file.document, gid, 1.0f, cf3(-2, 0, 0));
    add_sphere(file.document, lid, 1.0f, cf3(2, 0, 0));

    SUBCASE("undo restores the document exactly") {
        std::vector<std::uint8_t> before = scene::serialize_document(file.document);
        scene::UndoStack undo;
        CHECK(undo.perform(file.document,
                           scene::Command{scene::SetLayerProtectionCmd{gid, true, false}}));
        CHECK(file.document.find_layer(gid)->ghost);
        CHECK(undo.undo(file.document));
        CHECK_FALSE(file.document.find_layer(gid)->ghost);
        CHECK(scene::serialize_document(file.document) == before);
    }

    SUBCASE("both flags survive a save and load") {
        scene::apply(file.document,
                     scene::Command{scene::SetLayerProtectionCmd{gid, true, false}});
        scene::apply(file.document,
                     scene::Command{scene::SetLayerProtectionCmd{lid, false, true}});

        std::vector<std::uint8_t> bytes = io::save_clayspace(file);
        io::ClaySpaceDoc back;
        REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
        const scene::Layer* g = back.document.find_layer(gid);
        const scene::Layer* l = back.document.find_layer(lid);
        REQUIRE(g);
        REQUIRE(l);
        CHECK(g->ghost);
        CHECK_FALSE(g->locked);
        CHECK_FALSE(l->ghost);
        CHECK(l->locked);
        CHECK(g->visible);  // ghost is not hidden
        CHECK(l->visible);
    }

    SUBCASE("hidden and ghosted are independent") {
        scene::apply(file.document, scene::Command{scene::SetLayerVisibleCmd{gid, false}});
        scene::apply(file.document,
                     scene::Command{scene::SetLayerProtectionCmd{gid, true, false}});
        std::vector<std::uint8_t> bytes = io::save_clayspace(file);
        io::ClaySpaceDoc back;
        REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
        const scene::Layer* g = back.document.find_layer(gid);
        REQUIRE(g);
        CHECK_FALSE(g->visible);
        CHECK(g->ghost);
    }
}

// The flags are packed into the byte visibility already occupied, so a
// document written before they existed loads with both off, with no version
// handling at all. That is the whole reason for the packing, so it is checked.
TEST_CASE("protection: an older document loads unprotected") {
    scene::Document doc;
    const scene::LayerId a = doc.add_sdf_layer("visible").id;
    const scene::LayerId b = doc.add_sdf_layer("hidden").id;
    add_sphere(doc, a, 1.0f, cf3(0, 0, 0));
    add_sphere(doc, b, 1.0f, cf3(3, 0, 0));
    doc.find_layer(b)->visible = false;

    // What the previous format wrote: the same stream, with the flags byte
    // holding only 0 or 1 — which is what this document already produces,
    // since neither layer is protected.
    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    for (const scene::Layer& l : back->layers) {
        CHECK_FALSE(l.ghost);
        CHECK_FALSE(l.locked);
    }
    CHECK(back->layers[0].visible);
    CHECK_FALSE(back->layers[1].visible);
}

TEST_CASE("protection: edits refuse a protected layer") {
    scene::Document doc;
    const scene::LayerId layer = doc.add_sdf_layer("body").id;
    const scene::NodeId node = add_sphere(doc, layer, 1.0f, cf3(0, 0, 0));

    auto try_edits = [&](const char* what) {
        std::vector<std::uint8_t> before = scene::serialize_document(doc);

        scene::Node extra;
        extra.prim = scene::Prim::sphere(0.4f);
        extra.id = doc.find_layer(layer)->sdf->reserve_id();
        CHECK_FALSE(scene::apply(doc, scene::Command{scene::AddNodeCmd{
                                          layer, scene::kNoNode, -1, {extra}}}));
        CHECK_FALSE(scene::apply(
            doc, scene::Command{scene::SetTransformCmd{layer, node, math::Transform{}}}));
        CHECK_FALSE(scene::apply(
            doc, scene::Command{scene::SetColorCmd{layer, node, cf3(1, 0, 0)}}));
        CHECK_FALSE(scene::apply(doc, scene::Command{scene::RemoveNodeCmd{layer, node}}));
        CHECK_FALSE(scene::apply(doc, scene::Command{scene::RemoveLayerCmd{layer}}));
        CHECK_FALSE(scene::apply(doc, scene::Command{scene::SetLayerVisibleCmd{layer, false}}));

        INFO(what);
        CHECK(scene::serialize_document(doc) == before);  // nothing moved
    };

    SUBCASE("locked") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, false, true}});
        try_edits("locked");
    }
    SUBCASE("ghosted") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, true, false}});
        try_edits("ghosted");
    }

    SUBCASE("protection is reversible, or locking would be permanent") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, false, true}});
        CHECK_FALSE(
            scene::apply(doc, scene::Command{scene::SetColorCmd{layer, node, cf3(1, 0, 0)}}));
        // The protection command itself still lands on a protected layer.
        CHECK(scene::apply(doc,
                           scene::Command{scene::SetLayerProtectionCmd{layer, false, false}}));
        CHECK(scene::apply(doc, scene::Command{scene::SetColorCmd{layer, node, cf3(1, 0, 0)}}));
    }

    SUBCASE("an unprotected layer beside it still takes edits") {
        const scene::LayerId other = doc.add_sdf_layer("other").id;
        add_sphere(doc, other, 0.5f, cf3(4, 0, 0));
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, false, true}});
        CHECK(scene::apply(doc, scene::Command{scene::SetLayerVisibleCmd{other, false}}));
    }
}

TEST_CASE("protection: ghosted layers are not picked, locked ones are") {
    scene::Document doc;
    const scene::LayerId front = doc.add_sdf_layer("front").id;
    const scene::LayerId back = doc.add_sdf_layer("back").id;
    add_sphere(doc, front, 0.5f, cf3(0, 0, -2));  // nearer the camera at z = -6
    add_sphere(doc, back, 0.5f, cf3(0, 0, 2));

    math::Ray ray{cf3(0, 0, -6), cf3(0, 0, 1)};

    pick::SceneHit hit = pick::raycast_scene(doc, ray, {});
    REQUIRE(hit.hit);
    CHECK(hit.layer == front);

    SUBCASE("a ghost in front does not steal the hit") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{front, true, false}});
        pick::SceneHit after = pick::raycast_scene(doc, ray, {});
        REQUIRE(after.hit);
        CHECK(after.layer == back);
        // ...and the ghost is still in the field: the tape the picker uses is
        // not the tape the document evaluates to.
        CHECK(scene::compile_document(doc).eval(cf3(0, 0, -2)).d < 0.0f);
    }

    SUBCASE("ghosting everything means no hit") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{front, true, false}});
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{back, true, false}});
        CHECK_FALSE(pick::raycast_scene(doc, ray, {}).hit);
    }

    SUBCASE("a locked layer is still pickable") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{front, false, true}});
        pick::SceneHit after = pick::raycast_scene(doc, ray, {});
        REQUIRE(after.hit);
        CHECK(after.layer == front);
    }

    SUBCASE("attribution skips ghosts too") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{front, true, false}});
        scene::LayerId l = 0;
        scene::NodeId n = scene::kNoNode;
        pick::attribute(doc, cf3(0, 0, -1.5f), &l, &n);  // right at the ghost's surface
        CHECK(l != front);
    }
}

TEST_CASE("protection: the pickable tape only differs when a ghost exists") {
    scene::Document doc;
    const scene::LayerId layer = doc.add_sdf_layer("body").id;
    add_sphere(doc, layer, 1.0f, cf3(0, 0, 0));
    CHECK(pick::pickable_tape(doc).instrs.size() == scene::compile_document(doc).instrs.size());

    scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, true, false}});
    CHECK(pick::pickable_tape(doc).empty());
    CHECK_FALSE(scene::compile_document(doc).empty());
}

TEST_CASE("c protection: the flags across the ABI") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);

    const float radius[1] = {1.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    std::int32_t ghost = 1, locked = 1;
    REQUIRE(clay_document_layer_protection(doc, layer, &ghost, &locked) == CLAY_OK);
    CHECK(ghost == 0);
    CHECK(locked == 0);

    REQUIRE(clay_document_set_layer_protection(doc, layer, 0, 1) == CLAY_OK);
    REQUIRE(clay_document_layer_protection(doc, layer, &ghost, &locked) == CLAY_OK);
    CHECK(ghost == 0);
    CHECK(locked == 1);

    // An edit naming it is a typed error, not a silent drop.
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_visible(doc, layer, 0) == CLAY_ERROR_INVALID_ARGUMENT);

    // ...and the protection call itself still works, so this is reversible.
    REQUIRE(clay_document_set_layer_protection(doc, layer, 0, 0) == CLAY_OK);
    CHECK(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);

    CHECK(clay_document_layer_protection(doc, 999, &ghost, &locked) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_set_layer_protection(doc, 999, 1, 0) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_layer_protection(nullptr, layer, &ghost, &locked) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_item_destroy(item);
    clay_document_destroy(doc);
}
