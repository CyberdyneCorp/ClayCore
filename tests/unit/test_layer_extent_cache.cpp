// The layer extent, kept across edits (issue #451).
//
// An intersect is bounded by its layer's EXTENT, and computing that walks every
// visible node -- 96% of what the bound costs, paid on every edit because every
// edit might have changed it. `LayerExtentCache` keeps the extent AND the items
// achieving each of its six faces, so an edit to an item that holds no face out
// and still fits inside is decided with one item bound and six comparisons.
//
// THIS FILE IS THE EXHAUSTIVE GATE, and it is the reason the cache is allowed
// to exist at all. A cache that keeps an extent it should have dropped reports
// a bound that is too SMALL, and a bound too small is under-invalidation --
// stale bricks, no error, nothing in the result to notice. Twice already in
// this area a cache measured like a clean win and was silently wrong.
//
// So the gate is mechanical rather than careful: for EVERY command kind, apply
// it and then assert the cached extent equals a freshly computed one. A
// command added later that the cache does not understand fails here rather
// than in a host's viewport.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"

using namespace clay;
using namespace clay::scene;
using kernel::cf3;

namespace {

// A layer with room to move: items well inside the extent, and two at the
// edges so there is something that DOES define a face.
Document fixture(LayerId* out_layer, NodeId* out_inner, NodeId* out_edge) {
    Document doc;
    Layer& l = doc.add_sdf_layer("body");
    for (int i = 0; i < 12; ++i) {
        Node n;
        n.id = l.sdf->reserve_id();
        n.prim = Prim::sphere(0.2f);
        n.xform.position = cf3(0.1f * static_cast<float>(i), 0.0f, 0.0f);
        l.sdf->insert(n);
        if (i == 5) *out_inner = n.id;
    }
    Node edge;
    edge.id = l.sdf->reserve_id();
    edge.prim = Prim::sphere(0.3f);
    edge.xform.position = cf3(9.0f, 0.0f, 0.0f);  // the far +x face
    l.sdf->insert(edge);
    *out_edge = edge.id;
    *out_layer = l.id;
    return doc;
}

const Layer& layer_of(const Document& doc, LayerId id) {
    const Layer* l = doc.find_layer(id);
    REQUIRE(l != nullptr);
    return *l;
}

// The cache's answer against the walk's, which is the whole contract.
void check_agrees(LayerExtentCache& cache, const Document& doc, LayerId id) {
    const Layer& l = layer_of(doc, id);
    const math::Aabb cached = cache.of(*l.sdf, l);
    const math::Aabb fresh = layer_influence_extent(*l.sdf, l);
    CHECK(cached.min.x == fresh.min.x);
    CHECK(cached.min.y == fresh.min.y);
    CHECK(cached.min.z == fresh.min.z);
    CHECK(cached.max.x == fresh.max.x);
    CHECK(cached.max.y == fresh.max.y);
    CHECK(cached.max.z == fresh.max.z);
}

}  // namespace

TEST_CASE("layer extent cache: an item that holds no face out is free to move") {
    LayerId id = 0;
    NodeId inner = kNoNode, edge = kNoNode;
    Document doc = fixture(&id, &inner, &edge);

    LayerExtentCache cache;
    check_agrees(cache, doc, id);
    const std::size_t after_first = cache.walks();
    REQUIRE(after_first == 1);

    // Twenty frames of a drag, well inside the extent.
    for (int i = 0; i < 20; ++i) {
        Node* n = doc.find_layer(id)->sdf->find_mut(inner);
        REQUIRE(n != nullptr);
        n->xform.position = cf3(0.5f + 0.01f * static_cast<float>(i), 0.02f, 0.0f);
        const Layer& l = layer_of(doc, id);
        CHECK(cache.note_item_changed(*l.sdf, l, inner));
        check_agrees(cache, doc, id);
    }
    // Not one further walk in twenty frames, and the extent agreed with the
    // walk every time.
    CHECK(cache.walks() == after_first);
    CHECK(cache.keeps() == 20);
}

TEST_CASE("layer extent cache: an item that defines a face is not") {
    LayerId id = 0;
    NodeId inner = kNoNode, edge = kNoNode;
    Document doc = fixture(&id, &inner, &edge);

    LayerExtentCache cache;
    check_agrees(cache, doc, id);
    const std::size_t before = cache.walks();

    // Pulling the +x definer back must SHRINK the extent, and a union cannot
    // shrink -- so this has to walk rather than be kept.
    Node* n = doc.find_layer(id)->sdf->find_mut(edge);
    REQUIRE(n != nullptr);
    n->xform.position = cf3(1.0f, 0.0f, 0.0f);
    const Layer& l = layer_of(doc, id);
    CHECK_FALSE(cache.note_item_changed(*l.sdf, l, edge));
    check_agrees(cache, doc, id);
    CHECK(cache.walks() == before + 1);
}

TEST_CASE("layer extent cache: an item pushed outward grows the extent exactly") {
    LayerId id = 0;
    NodeId inner = kNoNode, edge = kNoNode;
    Document doc = fixture(&id, &inner, &edge);

    LayerExtentCache cache;
    check_agrees(cache, doc, id);

    // An inner item flung past every face. It defined none, so the union is
    // still a union -- kept, and it becomes the definer of what it now holds.
    Node* n = doc.find_layer(id)->sdf->find_mut(inner);
    REQUIRE(n != nullptr);
    n->xform.position = cf3(-30.0f, -30.0f, -30.0f);
    const Layer& l = layer_of(doc, id);
    CHECK(cache.note_item_changed(*l.sdf, l, inner));
    check_agrees(cache, doc, id);

    // And now it IS a definer, so pulling it back must walk.
    n = doc.find_layer(id)->sdf->find_mut(inner);
    n->xform.position = cf3(0.5f, 0.0f, 0.0f);
    CHECK_FALSE(cache.note_item_changed(*l.sdf, l, inner));
    check_agrees(cache, doc, id);
}

TEST_CASE("layer extent cache: every command kind leaves it agreeing with the walk") {
    // THE EXHAUSTIVE GATE. Each command is applied to a fresh fixture and the
    // cache is then asked what the extent is; it must equal what a walk says.
    //
    // The cache is told only what the wiring tells it -- here, deliberately,
    // NOTHING except for a transform edit, which is the case #451 is about.
    // Every other kind must therefore be invalidated wholesale, and this is
    // where a kind that was quietly assumed harmless shows up.
    LayerId id = 0;
    NodeId inner = kNoNode, edge = kNoNode;

    struct Case {
        const char* name;
        Command cmd;
        bool item_edit;  // whether the wiring may use the item fast path
    };

    Document probe = fixture(&id, &inner, &edge);
    const LayerId lid = id;
    Node spare;
    spare.id = probe.find_layer(lid)->sdf->reserve_id();
    spare.prim = Prim::sphere(0.25f);
    spare.xform.position = cf3(2.0f, 0.0f, 0.0f);

    std::vector<Case> cases;
    cases.push_back(Case{"SetTransform", Command{SetTransformCmd{lid, inner, math::Transform{}}}, true});
    {
        math::Transform t;
        t.position = cf3(40.0f, 0.0f, 0.0f);
        cases.push_back(Case{"SetTransform far", Command{SetTransformCmd{lid, inner, t}}, true});
    }
    cases.push_back(Case{"SetPrim", Command{SetPrimCmd{lid, inner, Prim::sphere(4.0f)}}, true});
    cases.push_back(Case{"SetColor", Command{SetColorCmd{lid, inner, cf3(1, 0, 0)}}, true});
    cases.push_back(Case{
        "SetOpBlend",
        Command{SetOpBlendCmd{lid, inner, Op::Add, Blend{BlendProfile::Quadratic, 2.0f}, 0.0f}},
        true});
    cases.push_back(Case{"RemoveNode", Command{RemoveNodeCmd{lid, edge}}, false});
    cases.push_back(Case{"AddNode", Command{AddNodeCmd{lid, kNoNode, -1, {spare}}}, false});
    cases.push_back(Case{"MoveNode", Command{MoveNodeCmd{lid, inner, kNoNode, 0}}, false});
    cases.push_back(Case{"SetLayerVisible", Command{SetLayerVisibleCmd{lid, false}}, false});
    {
        math::Transform t;
        t.position = cf3(5.0f, 5.0f, 5.0f);
        cases.push_back(Case{"SetLayerTransform", Command{SetLayerTransformCmd{lid, t}}, false});
    }
    cases.push_back(Case{"SetLayerMirror", Command{SetLayerMirrorCmd{lid, 1u, 0.0f}}, false});
    cases.push_back(Case{"SetLayerRadial", Command{SetLayerRadialCmd{lid, 4u, 1u, 0.0f}}, false});
    cases.push_back(Case{"SetDeformers", Command{SetDeformersCmd{lid, inner, {Deformer::twist(1.5f)}}}, true});

    for (Case& c : cases) {
        CAPTURE(std::string(c.name));
        LayerId fid = 0;
        NodeId finner = kNoNode, fedge = kNoNode;
        Document doc = fixture(&fid, &finner, &fedge);
        LayerExtentCache cache;
        {
            const Layer& l = layer_of(doc, fid);
            cache.of(*l.sdf, l);  // warm it
        }
        const std::optional<Command> inverse = scene::apply(doc, c.cmd);
        // A command the fixture refuses is not a case; every one here should
        // apply.
        REQUIRE(inverse.has_value());

        // The wiring's rule, stated here so the gate tests the rule and not a
        // reimplementation of it: an item edit may try the fast path, and
        // anything else invalidates.
        if (c.item_edit) {
            const Layer* l = doc.find_layer(fid);
            if (l && l->sdf) cache.note_item_changed(*l->sdf, *l, finner);
        } else {
            cache.invalidate();
        }
        check_agrees(cache, doc, fid);
    }
}
