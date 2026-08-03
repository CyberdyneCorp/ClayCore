#include <doctest/doctest.h>

#include "clay/scene/commands.h"
#include "kernel_utils.h"

using namespace clay;
using namespace clay::kernel;
using namespace clay::scene;

namespace {

Document base_document() {
    Document doc;
    Layer& l = doc.add_sdf_layer("body");
    Node n;
    n.prim = Prim::sphere(1.0f);
    l.sdf->insert(n);
    Node g;
    g.is_group = true;
    NodeId gid = l.sdf->insert(g);
    Node inner;
    inner.prim = Prim::box(cf3(0.5f, 0.5f, 0.5f));
    inner.xform.position = cf3(1, 0, 0);
    l.sdf->insert(inner, gid);
    Node stroke;
    stroke.prim = Prim::stroke();
    stroke.stroke = {{cf3(0, 0, 0), 0.2f}};
    l.sdf->insert(stroke);
    return doc;
}

NodeId nth_root(const Document& doc, std::size_t i) { return doc.layers[0].sdf->roots[i]; }

}  // namespace

TEST_CASE("every command's inverse restores the document bit-identically") {
    Document doc = base_document();
    LayerId lid = doc.layers[0].id;
    NodeId sphere = nth_root(doc, 0);
    NodeId group = nth_root(doc, 1);
    NodeId stroke = nth_root(doc, 2);

    Node fresh;
    fresh.prim = Prim::torus(1.0f, 0.2f);

    Layer extra;
    extra.id = 77;
    extra.name = "extra";
    extra.sdf = std::make_shared<SdfContent>();

    std::vector<Command> cmds = {
        AddNodeCmd{lid, kNoNode, 1, {fresh}},
        RemoveNodeCmd{lid, sphere},
        RemoveNodeCmd{lid, group},  // subtree removal
        MoveNodeCmd{lid, sphere, group, 0},
        SetTransformCmd{lid, sphere, math::Transform{cf3(1, 2, 3), math::Quat::identity(), 2.0f}},
        SetPrimCmd{lid, sphere, Prim::box(cf3(1, 1, 1))},
        SetColorCmd{lid, sphere, cf3(1, 0, 0)},
        SetOpBlendCmd{lid, sphere, Op::Subtract, Blend{BlendProfile::Cubic, 0.2f}, 0.05f},
        AppendStrokeCmd{lid, stroke, {{cf3(1, 1, 1), 0.3f}, {cf3(2, 1, 1), 0.25f}}},
        TrimStrokeCmd{lid, stroke, 1},
        AddLayerCmd{extra, -1},
        RemoveLayerCmd{lid},
        SetLayerVisibleCmd{lid, false},
        SetLayerTransformCmd{lid, math::Transform{cf3(0, 5, 0), math::Quat::identity(), 1.0f}},
    };

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        CAPTURE(i);
        Document work = base_document();
        std::vector<std::uint8_t> before = serialize_document(work);
        std::optional<Command> inverse = scene::apply(work, cmds[i]);
        REQUIRE(inverse.has_value());
        CHECK(serialize_document(work) != before);  // the command did something
        std::optional<Command> redo = scene::apply(work, *inverse);
        REQUIRE(redo.has_value());
        CHECK(serialize_document(work) == before);  // bit-identical restore
    }
}

TEST_CASE("apply on a missing target fails without touching the document") {
    Document doc = base_document();
    std::vector<std::uint8_t> before = serialize_document(doc);
    CHECK_FALSE(apply(doc, RemoveNodeCmd{doc.layers[0].id, 9999}).has_value());
    CHECK_FALSE(apply(doc, SetColorCmd{999, 1, cf3(1, 0, 0)}).has_value());
    CHECK(serialize_document(doc) == before);
}

TEST_CASE("every command serializes and deserializes losslessly") {
    Document doc = base_document();
    LayerId lid = doc.layers[0].id;
    NodeId sphere = nth_root(doc, 0);

    Node fresh;
    fresh.prim = Prim::capsule(cf3(0, 0, 0), cf3(0, 1, 0), 0.2f);
    fresh.stroke = {{cf3(0, 0, 0), 0.1f}};

    Layer extra;
    extra.id = 42;
    extra.name = "serialized-layer";
    extra.sdf = doc.layers[0].sdf;  // content with nodes

    std::vector<Command> cmds = {
        AddNodeCmd{lid, kNoNode, 0, {fresh}},
        RemoveNodeCmd{lid, sphere},
        MoveNodeCmd{lid, sphere, kNoNode, 2},
        SetTransformCmd{lid, sphere,
                        math::Transform{cf3(1, 2, 3),
                                        math::Quat::from_axis_angle(cf3(0, 1, 0), 0.5f), 1.5f}},
        SetPrimCmd{lid, sphere, Prim::hex_prism(0.4f, 0.3f)},
        SetColorCmd{lid, sphere, cf3(0.1f, 0.2f, 0.3f)},
        SetOpBlendCmd{lid, sphere, Op::Paint, Blend{BlendProfile::Chamfer, 0.15f}, -0.02f},
        AppendStrokeCmd{lid, sphere, {{cf3(4, 5, 6), 0.7f}}},
        TrimStrokeCmd{lid, sphere, 3},
        AddLayerCmd{extra, 1},
        RemoveLayerCmd{lid},
        SetLayerVisibleCmd{lid, false},
        SetLayerTransformCmd{lid, math::Transform{}},
    };

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        CAPTURE(i);
        std::vector<std::uint8_t> bytes = serialize(cmds[i]);
        std::optional<Command> back = deserialize(bytes.data(), bytes.size());
        REQUIRE(back.has_value());
        CHECK(serialize(*back) == bytes);  // canonical round trip
        CHECK(back->index() == cmds[i].index());
    }
}

TEST_CASE("deserialize rejects truncated and garbage input") {
    std::vector<std::uint8_t> bytes = serialize(RemoveNodeCmd{1, 2});
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
        CHECK_FALSE(deserialize(bytes.data(), cut).has_value());
    std::uint8_t garbage[] = {0xFF, 0x00, 0x12};
    CHECK_FALSE(deserialize(garbage, sizeof garbage).has_value());
}

TEST_CASE("undo/redo round trip") {
    Document doc = base_document();
    std::vector<std::uint8_t> initial = serialize_document(doc);
    UndoStack stack;
    LayerId lid = doc.layers[0].id;
    NodeId sphere = nth_root(doc, 0);

    REQUIRE(stack.perform(doc, SetColorCmd{lid, sphere, cf3(1, 0, 0)}));
    std::vector<std::uint8_t> after_color = serialize_document(doc);
    REQUIRE(stack.perform(doc, SetPrimCmd{lid, sphere, Prim::sphere(2.0f)}));

    CHECK(stack.undo(doc));
    CHECK(serialize_document(doc) == after_color);
    CHECK(stack.undo(doc));
    CHECK(serialize_document(doc) == initial);
    CHECK_FALSE(stack.undo(doc));

    CHECK(stack.redo(doc));
    CHECK(serialize_document(doc) == after_color);
    CHECK(stack.redo(doc));
    CHECK_FALSE(stack.redo(doc));
}

TEST_CASE("stroke commands coalesce into one undo step") {
    Document doc = base_document();
    UndoStack stack;
    LayerId lid = doc.layers[0].id;
    NodeId stroke = nth_root(doc, 2);
    std::vector<std::uint8_t> initial = serialize_document(doc);

    for (int i = 0; i < 25; ++i) {
        float x = static_cast<float>(i) * 0.1f;
        REQUIRE(stack.perform(doc, AppendStrokeCmd{lid, stroke, {{cf3(x, 0, 0), 0.2f}}}));
    }
    CHECK(doc.layers[0].sdf->find(stroke)->stroke.size() == 26);  // 1 + 25
    CHECK(stack.undo_depth() == 1);  // coalesced
    CHECK(stack.undo(doc));
    CHECK(serialize_document(doc) == initial);
}

TEST_CASE("begin/end group bundles commands into one step") {
    Document doc = base_document();
    UndoStack stack;
    LayerId lid = doc.layers[0].id;
    NodeId sphere = nth_root(doc, 0);
    std::vector<std::uint8_t> initial = serialize_document(doc);

    stack.begin_group();
    REQUIRE(stack.perform(doc, SetColorCmd{lid, sphere, cf3(1, 0, 0)}));
    REQUIRE(stack.perform(doc, SetPrimCmd{lid, sphere, Prim::sphere(3.0f)}));
    REQUIRE(stack.perform(
        doc, SetTransformCmd{lid, sphere, math::Transform{cf3(9, 9, 9), {}, 1.0f}}));
    stack.end_group();

    CHECK(stack.undo_depth() == 1);
    CHECK(stack.undo(doc));
    CHECK(serialize_document(doc) == initial);
    CHECK(stack.redo(doc));
    CHECK(serialize_document(doc) != initial);
}

TEST_CASE("new command clears the redo stack") {
    Document doc = base_document();
    UndoStack stack;
    LayerId lid = doc.layers[0].id;
    NodeId sphere = nth_root(doc, 0);
    REQUIRE(stack.perform(doc, SetColorCmd{lid, sphere, cf3(1, 0, 0)}));
    REQUIRE(stack.undo(doc));
    CHECK(stack.redo_depth() == 1);
    REQUIRE(stack.perform(doc, SetColorCmd{lid, sphere, cf3(0, 1, 0)}));
    CHECK(stack.redo_depth() == 0);
}
