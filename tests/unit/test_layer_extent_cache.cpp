// The layer extent, kept across edits (issue #451).
//
// An intersect is bounded by its layer's EXTENT, and computing that walks every
// visible node -- 96% of what the bound costs, paid on every edit because every
// edit might have changed it. `LayerExtentCache` holds the union of every item
// EXCEPT one, so an edit to that one is a single item bound and a union.
//
// WHY EXCEPT-ONE AND NOT SOMETHING CLEVERER. The first attempt kept the extent
// plus the item achieving each of its six faces, letting anything that held no
// face out move freely inside. It measured as 201 walks over a 200-frame drag,
// because what a host drags across a form is a boolean OPERAND and an operand
// big enough to cut something sticks out of it. `a dragged cutter sticks out`
// below is that fixture, and it is the case to keep working.
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
Document fixture(LayerId* out_layer, NodeId* out_inner, NodeId* out_edge,
                 NodeId* out_stroke = nullptr) {
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
    // A stroke, so the stroke-shaped commands have something they can edit.
    Node stroke;
    stroke.id = l.sdf->reserve_id();
    stroke.prim = Prim::stroke();
    for (int k = 0; k < 4; ++k) {
        StrokePoint sp;
        sp.pos = cf3(0.2f * static_cast<float>(k), 0.1f, 0.0f);
        sp.radius = 0.08f;
        stroke.stroke.push_back(sp);
    }
    l.sdf->insert(stroke);
    if (out_stroke) *out_stroke = stroke.id;

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

TEST_CASE("layer extent cache: a drag pays one walk and then none") {
    LayerId id = 0;
    NodeId inner = kNoNode, edge = kNoNode;
    Document doc = fixture(&id, &inner, &edge);

    LayerExtentCache cache;
    check_agrees(cache, doc, id);
    REQUIRE(cache.walks() == 1);

    // Twenty frames of a drag. The first chooses whom to hold out and walks;
    // the nineteen after it are answered from what that walk left behind.
    for (int i = 0; i < 20; ++i) {
        Node* n = doc.find_layer(id)->sdf->find_mut(inner);
        REQUIRE(n != nullptr);
        n->xform.position = cf3(0.5f + 0.01f * static_cast<float>(i), 0.02f, 0.0f);
        const Layer& l = layer_of(doc, id);
        const bool kept = cache.note_item_changed(*l.sdf, l, inner);
        CHECK(kept == (i > 0));
        check_agrees(cache, doc, id);
    }
    CHECK(cache.walks() == 2);
    CHECK(cache.keeps() == 19);
}

TEST_CASE("layer extent cache: a dragged cutter sticks out and is still free") {
    // THE CASE THE FIRST DESIGN FAILED, and the shape of #451: the dragged item
    // defines a face of the extent on every single frame, because it is the
    // boolean operand being swept across the form. Under the old rule that
    // meant a walk per frame. Here it is the item held out, so it does not.
    LayerId id = 0;
    NodeId inner = kNoNode, edge = kNoNode;
    Document doc = fixture(&id, &inner, &edge);

    LayerExtentCache cache;
    check_agrees(cache, doc, id);

    for (int i = 0; i < 20; ++i) {
        Node* n = doc.find_layer(id)->sdf->find_mut(edge);  // the +x definer
        REQUIRE(n != nullptr);
        n->xform.position = cf3(9.0f + 0.5f * static_cast<float>(i), 0.0f, 0.0f);
        const Layer& l = layer_of(doc, id);
        CHECK(cache.note_item_changed(*l.sdf, l, edge) == (i > 0));
        check_agrees(cache, doc, id);
    }
    CHECK(cache.walks() == 2);
}

TEST_CASE("layer extent cache: the held-out item shrinking shrinks the extent") {
    // A union cannot shrink, so a cache that kept the extent and expanded it
    // would be WRONG here and wrong in the direction that does not announce
    // itself -- a bound too large is merely slow, a bound built by expanding a
    // stale union is too small the moment the thing that made it big moves in.
    //
    // Holding the item out is what makes this exact: its old box was never in
    // `rest_`, so there is nothing stale to expand.
    LayerId id = 0;
    NodeId inner = kNoNode, edge = kNoNode;
    Document doc = fixture(&id, &inner, &edge);

    LayerExtentCache cache;
    check_agrees(cache, doc, id);
    const Layer& l = layer_of(doc, id);

    // Fling the far item further out, then haul it back inside the others.
    Node* n = doc.find_layer(id)->sdf->find_mut(edge);
    n->xform.position = cf3(40.0f, 0.0f, 0.0f);
    cache.note_item_changed(*l.sdf, l, edge);  // the walk that holds it out
    check_agrees(cache, doc, id);
    const math::Aabb grown = cache.of(*l.sdf, l);
    CHECK(grown.max.x > 39.0f);

    n = doc.find_layer(id)->sdf->find_mut(edge);
    n->xform.position = cf3(0.4f, 0.0f, 0.0f);
    CHECK(cache.note_item_changed(*l.sdf, l, edge));  // kept, and it must SHRINK
    check_agrees(cache, doc, id);
    CHECK(cache.of(*l.sdf, l).max.x < 2.0f);

    // Also shrinking it by making the primitive smaller rather than moving it.
    n = doc.find_layer(id)->sdf->find_mut(edge);
    n->prim = Prim::sphere(0.01f);
    CHECK(cache.note_item_changed(*l.sdf, l, edge));
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

    NodeId stroke = kNoNode;
    Document probe = fixture(&id, &inner, &edge, &stroke);
    const LayerId lid = id;
    Node spare;
    spare.id = probe.find_layer(lid)->sdf->reserve_id();
    spare.prim = Prim::sphere(0.25f);
    spare.xform.position = cf3(2.0f, 0.0f, 0.0f);

    std::vector<Case> cases;
    cases.push_back(
        Case{"SetTransform", Command{SetTransformCmd{lid, inner, math::Transform{}}}, true});
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
    cases.push_back(
        Case{"SetDeformers", Command{SetDeformersCmd{lid, inner, {Deformer::twist(1.5f)}}}, true});
    cases.push_back(
        Case{"SetLayerProtection", Command{SetLayerProtectionCmd{lid, false, false}}, false});
    cases.push_back(Case{"SetLayerName", Command{SetLayerNameCmd{lid, "renamed"}}, false});
    {  // a stroke item, so the stroke-shaped commands have something to edit
        std::vector<StrokePoint> pts;
        for (int k = 0; k < 4; ++k) {
            StrokePoint sp;
            sp.pos = cf3(0.3f * static_cast<float>(k), 0.0f, 0.0f);
            sp.radius = 0.1f;
            pts.push_back(sp);
        }
        cases.push_back(Case{"AppendStroke", Command{AppendStrokeCmd{lid, stroke, pts}}, true});
        cases.push_back(Case{
            "SetStrokePoints", Command{SetStrokePointsCmd{lid, stroke, pts, false, 0.01f}}, true});
        cases.push_back(Case{"TrimStroke", Command{TrimStrokeCmd{lid, stroke, 1u}}, true});
    }

    for (Case& c : cases) {
        CAPTURE(std::string(c.name));
        LayerId fid = 0;
        NodeId finner = kNoNode, fedge = kNoNode, fstroke = kNoNode;
        Document doc = fixture(&fid, &finner, &fedge, &fstroke);
        LayerExtentCache cache;
        {
            const Layer& l = layer_of(doc, fid);
            cache.of(*l.sdf, l);  // warm it
        }
        const std::optional<Command> inverse = scene::apply(doc, c.cmd);
        // A command the fixture refuses is not a case; every one here should
        // apply.
        REQUIRE(inverse.has_value());

        // THE REAL RULE, called rather than restated. `command_edited_item` is
        // what `apply_edit` uses to decide, so this gate tests the wiring and
        // not a second copy of it that could agree with a bug.
        const EditedItem edited = command_edited_item(c.cmd);
        CHECK(edited.known == c.item_edit);  // and the expectation is pinned too
        const Layer* el = edited.known ? doc.find_layer(edited.layer) : nullptr;
        if (el && el->sdf)
            cache.note_item_changed(*el->sdf, *el, edited.node);
        else
            cache.invalidate();
        check_agrees(cache, doc, fid);
    }
}
