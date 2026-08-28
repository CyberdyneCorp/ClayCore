#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>

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
        SetLayerNameCmd{lid, "renomeada"},
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
        SetLayerNameCmd{lid, "renomeada"},
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

// accel/undo-removal: SdfContent keeps an id -> (parent, index) location
// index so locate() — paid once per node an undo removes — no longer walks
// the whole arena. The two cases below pin what the index must never change:
// undo/redo semantics and serialized bytes at sculpt-sized documents, and
// locate() answers under every structural edit including the paths that
// bypass the index (the document reader's wholesale roots rewrite, and a
// host mutating the public `roots` member directly).

namespace {

Node stamp_node(SdfContent& c, int i) {
    Node n;
    n.id = c.reserve_id();
    n.prim = Prim::sphere(0.05f);
    n.xform.position = cf3(0.001f * static_cast<float>(i), 0.002f * static_cast<float>(i), 0);
    return n;
}

// The pre-index locate, kept here as the reference the indexed one must match.
bool reference_locate(const SdfContent& c, NodeId id, NodeId* parent, int* index) {
    for (std::size_t i = 0; i < c.roots.size(); ++i)
        if (c.roots[i] == id) {
            *parent = kNoNode;
            *index = static_cast<int>(i);
            return true;
        }
    for (const auto& [nid, n] : c.nodes())
        for (std::size_t i = 0; i < n.children.size(); ++i)
            if (n.children[i] == id) {
                *parent = nid;
                *index = static_cast<int>(i);
                return true;
            }
    return false;
}

void check_locate_matches_reference(const SdfContent& c) {
    for (const auto& [id, n] : c.nodes()) {
        CAPTURE(id);
        NodeId want_parent = kNoNode, got_parent = kNoNode;
        int want_index = -1, got_index = -1;
        bool want = reference_locate(c, id, &want_parent, &want_index);
        bool got = c.locate(id, &got_parent, &got_index);
        CHECK(got == want);
        CHECK(got_parent == want_parent);
        CHECK(got_index == want_index);
    }
}

}  // namespace

TEST_CASE("undoing a stroke on a 10k-stamp document is byte-exact") {
    Document doc;
    Layer& l = doc.add_sdf_layer("sculpt");
    LayerId lid = l.id;
    UndoStack stack;
    for (int i = 0; i < 10000; ++i)
        REQUIRE(stack.perform(doc, AddNodeCmd{lid, kNoNode, -1, {stamp_node(*l.sdf, i)}}));
    std::vector<std::uint8_t> base = serialize_document(doc);

    stack.begin_group();
    for (int i = 0; i < 100; ++i)
        REQUIRE(stack.perform(doc, AddNodeCmd{lid, kNoNode, -1, {stamp_node(*l.sdf, 20000 + i)}}));
    stack.end_group();
    std::vector<std::uint8_t> stroked = serialize_document(doc);
    CHECK(stroked != base);

    REQUIRE(stack.undo(doc));
    CHECK(serialize_document(doc) == base);
    REQUIRE(stack.redo(doc));
    CHECK(serialize_document(doc) == stroked);
    REQUIRE(stack.undo(doc));
    CHECK(serialize_document(doc) == base);
}

TEST_CASE("locate stays exact through structural churn and index bypasses") {
    Document doc = base_document();
    SdfContent& c = *doc.layers[0].sdf;
    NodeId group = nth_root(doc, 1);

    // Grow, then churn: inserts at indices, same-list and cross-list moves,
    // refused moves (into a non-group, into the node's own subtree), a
    // middle removal and its reinsert.
    for (int i = 0; i < 20; ++i) c.insert(stamp_node(c, i), i % 3 == 0 ? group : kNoNode, i % 5);
    check_locate_matches_reference(c);
    NodeId item_a = c.insert(stamp_node(c, 100));
    NodeId item_b = c.insert(stamp_node(c, 101));
    REQUIRE(c.move(c.roots[7], kNoNode, 2));
    REQUIRE(c.move(c.roots[0], group, -1));
    CHECK_FALSE(c.move(item_a, item_b, 0));                     // dest is not a group
    CHECK_FALSE(c.move(group, c.find(group)->children[0], 0));  // own subtree
    check_locate_matches_reference(c);
    NodeId doomed = c.roots[3];
    NodeId parent = kNoNode;
    int index = -1;
    REQUIRE(c.locate(doomed, &parent, &index));
    std::vector<Node> cut = c.remove(doomed);
    REQUIRE_FALSE(cut.empty());
    NodeId gone_parent = kNoNode;
    int gone_index = -1;
    CHECK_FALSE(c.locate(doomed, &gone_parent, &gone_index));
    REQUIRE(c.reinsert(cut, parent, index));
    check_locate_matches_reference(c);

    // The document reader rewrites `roots` wholesale rather than editing
    // through insert/remove/move; locate must be exact on the loaded copy.
    std::vector<std::uint8_t> bytes = serialize_document(doc);
    std::optional<Document> back = deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    check_locate_matches_reference(*back->layers[0].sdf);
    CHECK(serialize_document(*back) == bytes);

    // A host can mutate the public member directly; a stale index entry must
    // fall back to the walk and self-repair, never answer wrongly.
    std::reverse(c.roots.begin(), c.roots.end());
    check_locate_matches_reference(c);
    check_locate_matches_reference(c);  // again, through the repaired entries
}

TEST_CASE("the undo bound covers every layer sharing the content") {
    // Layer instancing shares SdfContent by reference, so ONE command lands
    // once per instance, each through that layer's own transform. A bound
    // taken against the layer the command names would leave the other
    // instance's copy of the edit stale — and nothing at the boundary could
    // detect it, because both instances hold the same node id.
    Document doc;
    Layer& src = doc.add_sdf_layer("body");
    LayerId lid = src.id;
    Layer* copy = doc.instance_layer(lid, "body copy");
    REQUIRE(copy != nullptr);
    copy->xform.position = cf3(10, 0, 0);

    UndoStack stack;
    Node n;
    n.id = doc.layers[0].sdf->reserve_id();
    n.prim = Prim::sphere(0.5f);
    REQUIRE(stack.perform(doc, AddNodeCmd{lid, kNoNode, -1, {n}}));

    math::Aabb bound;
    REQUIRE(stack.undo(doc, &bound));
    REQUIRE_FALSE(bound.empty());
    CHECK_FALSE(bound.is_infinite());
    CHECK(bound.contains(cf3(0, 0, 0)));   // where the source layer draws it
    CHECK(bound.contains(cf3(10, 0, 0)));  // and where the instance does
}

TEST_CASE("the undo bound is opt-in and changes nothing when it is not asked for") {
    // The bound is extra information on the same call: passing no bound must
    // leave the document exactly where passing one leaves it.
    Document quiet = base_document();
    Document loud = base_document();
    UndoStack quiet_stack;
    UndoStack loud_stack;
    LayerId lid = quiet.layers[0].id;

    auto edit = [&](Document& doc, UndoStack& stack) {
        REQUIRE(stack.perform(doc, SetTransformCmd{lid, nth_root(doc, 0), math::Transform{}}));
        REQUIRE(stack.perform(doc, RemoveNodeCmd{lid, nth_root(doc, 2)}));
    };
    edit(quiet, quiet_stack);
    edit(loud, loud_stack);

    math::Aabb bound;
    for (int i = 0; i < 2; ++i) {
        REQUIRE(quiet_stack.undo(quiet));
        REQUIRE(loud_stack.undo(loud, &bound));
    }
    CHECK(serialize_document(quiet) == serialize_document(loud));
    CHECK_FALSE(bound.empty());
}

TEST_CASE("a shared edit list serializes once and reloads shared") {
    // The scene payload at minor 15 (instance-a-layer): a layer record names
    // the layer whose edit list it shares instead of repeating it. Written at
    // an older minor the content goes out inline, which is the recoverable
    // degradation — the shapes are all there and the sharing is not.
    Document doc;
    Layer& src = doc.add_sdf_layer("body");
    const LayerId lid = src.id;
    for (int i = 0; i < 40; ++i) {
        Node n;
        n.id = src.sdf->reserve_id();
        n.prim = Prim::sphere(0.2f);
        n.xform.position = cf3(0.1f * static_cast<float>(i), 0, 0);
        src.sdf->insert(n);
    }
    Layer* copy = doc.instance_layer(lid, "body copy");
    REQUIRE(copy != nullptr);
    const LayerId cid = copy->id;

    Document plain;
    Layer& only = plain.add_sdf_layer("body");
    only.sdf = doc.layers[0].sdf;

    const std::vector<std::uint8_t> shared = serialize_document(doc);
    const std::vector<std::uint8_t> single = serialize_document(plain);
    // Non-degenerate: a comparison against a handful of bytes means nothing.
    REQUIRE(single.size() > 1000);
    // One edit list on the wire, not two.
    CHECK(shared.size() < single.size() * 3 / 2);

    std::optional<Document> back = deserialize_document(shared.data(), shared.size());
    REQUIRE(back.has_value());
    REQUIRE(back->layers.size() == 2);
    CHECK(back->layers[0].sdf == back->layers[1].sdf);
    // Shared, not merely equal: an edit through one is an edit through both.
    Node extra;
    extra.id = back->find_layer(cid)->sdf->reserve_id();
    extra.prim = Prim::sphere(0.2f);
    back->find_layer(cid)->sdf->insert(extra);
    CHECK(back->find_layer(lid)->sdf->roots.size() == 41);

    // At minor 14 the sharing is what is lost, and only that: two independent
    // layers, each carrying the whole edit list.
    const std::vector<std::uint8_t> old = serialize_document(doc, 14);
    CHECK(old.size() > single.size() * 3 / 2);
    std::optional<Document> older = deserialize_document(old.data(), old.size(), 14);
    REQUIRE(older.has_value());
    REQUIRE(older->layers.size() == 2);
    CHECK(older->layers[0].sdf != older->layers[1].sdf);
    CHECK(older->layers[0].sdf->roots.size() == 40);
    CHECK(older->layers[1].sdf->roots.size() == 40);
}

TEST_CASE("a document naming content it does not carry is refused") {
    // Rather than opened with an empty layer where an instance was: an artist
    // reading "the subtool is empty" would take it for their own work lost.
    Document doc;
    Layer& src = doc.add_sdf_layer("body");
    Node n;
    n.id = src.sdf->reserve_id();
    n.prim = Prim::sphere(0.5f);
    src.sdf->insert(n);
    Layer* copy = doc.instance_layer(src.id, "body copy");
    REQUIRE(copy != nullptr);

    std::vector<std::uint8_t> bytes = serialize_document(doc);
    REQUIRE(deserialize_document(bytes.data(), bytes.size()).has_value());
    // A sharing layer's record ENDS with its content source and the content
    // flag, and it carries no content — so the last five bytes of the stream
    // are that pair, and the source id is the first four of them. Addressed
    // by position rather than by searching for the value, which a node id or
    // a float would collide with.
    REQUIRE(bytes.size() > 5);
    const LayerId missing = 9999;
    std::memcpy(bytes.data() + bytes.size() - 5, &missing, sizeof(missing));
    CHECK_FALSE(deserialize_document(bytes.data(), bytes.size()).has_value());
}

TEST_CASE("a layer-add naming a source that is gone is refused") {
    // The replay half of the same rule: an AddLayerCmd that names its content
    // rather than carrying it must not fall back to an empty or copied edit
    // list, which would unlink the layers where nothing can see it.
    Document doc;
    Layer& src = doc.add_sdf_layer("body");
    Node n;
    n.id = src.sdf->reserve_id();
    n.prim = Prim::sphere(0.5f);
    src.sdf->insert(n);

    Layer named;
    named.id = doc.reserve_layer_id();
    named.name = "instance";
    CHECK_FALSE(clay::scene::apply(doc, Command{AddLayerCmd{named, -1, 4242}}).has_value());
    CHECK(doc.layers.size() == 1);

    // And it resolves against a source that IS there.
    REQUIRE(clay::scene::apply(doc, Command{AddLayerCmd{named, -1, src.id}}).has_value());
    REQUIRE(doc.layers.size() == 2);
    CHECK(doc.layers[1].sdf == doc.layers[0].sdf);
}
