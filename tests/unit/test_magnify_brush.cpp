// The Magnify/Pinch brush on a layer's assembled surface (brush-engine spec,
// add-magnify-surface-brush; issue #391).
//
// `cdeform_magnify` is per item and local, exactly as `grab` is, and the Move
// resolver exists because of that. Magnify had no such counterpart, so Pinch
// could not be a surface brush on a field at all — a host could only put a
// magnify on one picked item and watch one side of a blended form gather while
// the rest stayed put.
//
// The claims worth defending: a blended form pinches as ONE surface, the sign
// means what the header says it means, the region is world-space and travels
// through a layer transform, a gesture replaces its own last frame rather than
// stacking, and under a mirror the gesture and its reflection agree bit for
// bit.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "clay/brush/magnify.h"
#include "clay/brush/move.h"
#include "clay/math/transform.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using brush::MagnifySettings;
using brush::MoveWarp;
using kernel::cf3;
using kernel::cfloat3;
using namespace clay::scene;

namespace {

// Two balls smooth-unioned into one form: the case a per-item magnify gets
// wrong, because the region under the cursor belongs to more than one item.
Document two_balls(float gap = 0.45f) {
    Document doc;
    Layer layer;
    layer.id = 1;
    layer.kind = LayerKind::Sdf;
    layer.sdf = std::make_shared<SdfContent>();
    for (float x : {-gap, gap}) {
        Node ball;
        ball.prim = Prim::sphere(0.5f);
        ball.op = Op::Add;
        ball.blend = Blend{BlendProfile::Quadratic, 0.25f};
        ball.xform.position = cf3(x, 0, 0);
        layer.sdf->insert(ball);
    }
    doc.layers.push_back(layer);
    return doc;
}

// Where the surface sits along +Y above a given x, marching down from outside.
float surface_y(const Document& doc, float x) {
    Tape t = compile_document(doc);
    float last = 1.0f;
    for (float y = 1.6f; y > -1.6f; y -= 0.002f) {
        const float d = t.eval(cf3(x, y, 0)).d;
        if (d <= 0.0f && last > 0.0f) return y;
        last = d;
    }
    return 0.0f;
}

std::size_t apply_warps(Document& doc, LayerId layer, const std::vector<MoveWarp>& warps) {
    SdfContent* content = doc.find_layer(layer)->sdf.get();
    std::size_t applied = 0;
    for (const MoveWarp& w : warps) {
        const Node* n = content->find(w.node);
        if (!n) continue;
        SetDeformersCmd cmd{layer, w.node, brush::moved_chain(*n, w)};
        if (scene::apply(doc, Command{cmd})) ++applied;
    }
    return applied;
}

}  // namespace

// -- the thing a magnify on one item cannot do --------------------------------

TEST_CASE("magnify: a blended form swells as one surface") {
    const Document before = two_balls();
    const float base_left = surface_y(before, -0.45f);
    const float base_right = surface_y(before, 0.45f);

    Document doc = two_balls();
    MagnifySettings s;
    s.radius = 0.8f;
    const std::vector<MoveWarp> warps = brush::magnify_brush(doc.layers[0], cf3(0, 0, 0), 0.4f, s);
    REQUIRE(warps.size() == 2);  // both items take a share
    CHECK(apply_warps(doc, 1, warps) == 2);

    const float left = surface_y(doc, -0.45f) - base_left;
    const float right = surface_y(doc, 0.45f) - base_right;
    CHECK(left > 0.0f);
    CHECK(right > 0.0f);
    // Symmetric about the centre of the gesture, which is what "one surface"
    // means here: neither ball is favoured by having been picked.
    CHECK(left == doctest::Approx(right).epsilon(0.1));
}

TEST_CASE("magnify: a deformer on one item is NOT the same thing") {
    // The whole reason this resolver exists, and the measurement issue #391
    // asks for. Same deformation, put on one item by hand: its side moves and
    // the other is left behind.
    const Document before = two_balls();
    Document doc = two_balls();
    Node* left = doc.layers[0].sdf->find_mut(doc.layers[0].sdf->roots[0]);
    left->deformers.push_back(Deformer::magnify(cf3(0, 0, 0), 0.8f, 0.4f));

    const float moved_left = surface_y(doc, -0.45f) - surface_y(before, -0.45f);
    const float moved_right = surface_y(doc, 0.45f) - surface_y(before, 0.45f);
    CHECK(std::abs(moved_left) > 0.01f);
    CHECK(moved_right == doctest::Approx(0.0f).epsilon(0.05));
}

TEST_CASE("magnify: the sign is the difference between Magnify and Pinch") {
    // One signed parameter rather than two verbs, and the directions are the
    // ones the deformer's own doc claims: positive swells away from the centre,
    // negative gathers toward it.
    const float base = surface_y(two_balls(), 0.0f);

    Document swelled = two_balls();
    MagnifySettings s;
    s.radius = 0.8f;
    apply_warps(swelled, 1, brush::magnify_brush(swelled.layers[0], cf3(0, 0, 0), 0.4f, s));

    Document gathered = two_balls();
    apply_warps(gathered, 1, brush::magnify_brush(gathered.layers[0], cf3(0, 0, 0), -0.4f, s));

    CHECK(surface_y(swelled, 0.0f) > base);
    CHECK(surface_y(gathered, 0.0f) < base);
}

TEST_CASE("magnify: the centre is in WORLD space, not the item's") {
    // A magnify's own centre is local, which is the trap this resolver exists
    // to close. The item sits at +0.45, so a world gesture at the origin lands
    // at -0.45 in its frame; a resolver that passed the world centre through
    // would scale about a point half a ball away.
    Document doc = two_balls();
    MagnifySettings s;
    s.radius = 0.8f;
    const std::vector<MoveWarp> warps = brush::magnify_brush(doc.layers[0], cf3(0, 0, 0), 0.4f, s);
    REQUIRE(warps.size() == 2);

    for (const MoveWarp& w : warps) {
        REQUIRE(w.deformers.size() == 1);
        const Deformer& d = w.deformers[0];
        CHECK(d.type == kernel::cdeform_magnify);
        const Node* n = doc.layers[0].sdf->find(w.node);
        REQUIRE(n != nullptr);
        // The world origin, expressed in the item's own frame.
        CHECK(d.k == doctest::Approx(-n->xform.position.x));
        CHECK(d.ext[0] == doctest::Approx(0.4f));  // the strength crosses untouched
    }
}

TEST_CASE("magnify: a layer transform is mapped through") {
    // The layer places the items, so a world gesture has to cross that
    // placement too. Scaled by two and shifted, the radius the item sees is
    // halved and the centre lands where the layer's inverse puts it.
    Document doc = two_balls();
    doc.layers[0].xform.position = cf3(1.0f, 0.0f, 0.0f);
    doc.layers[0].xform.scale = 2.0f;

    MagnifySettings s;
    s.radius = 0.8f;
    const std::vector<MoveWarp> warps =
        brush::magnify_brush(doc.layers[0], cf3(1.0f, 0.0f, 0.0f), 0.4f, s);
    REQUIRE(!warps.empty());
    for (const MoveWarp& w : warps) {
        REQUIRE(w.deformers.size() == 1);
        CHECK(w.deformers[0].c == doctest::Approx(0.4f));  // 0.8 world / scale 2
    }
}

TEST_CASE("magnify: items out of reach take no warp") {
    // Finite support: outside the radius the weight is zero and the field is
    // untouched, so a deformer there is a tape record that costs every
    // evaluation and changes nothing.
    Document doc = two_balls(2.5f);  // far apart
    MagnifySettings s;
    s.radius = 0.4f;
    const std::vector<MoveWarp> warps =
        brush::magnify_brush(doc.layers[0], cf3(-2.5f, 0, 0), 0.4f, s);
    CHECK(warps.size() == 1);
}

TEST_CASE("magnify: refusals produce nothing") {
    Document doc = two_balls();
    MagnifySettings s;
    s.radius = 0.8f;
    // A strength of zero scales by one.
    CHECK(brush::magnify_brush(doc.layers[0], cf3(0, 0, 0), 0.0f, s).empty());
    // A non-positive radius is not a region.
    MagnifySettings bad;
    bad.radius = 0.0f;
    CHECK(brush::magnify_brush(doc.layers[0], cf3(0, 0, 0), 0.4f, bad).empty());
    // A layer with no edit list has nothing to gather.
    Layer empty;
    empty.id = 2;
    empty.kind = LayerKind::Sdf;
    CHECK(brush::magnify_brush(empty, cf3(0, 0, 0), 0.4f, s).empty());
}

TEST_CASE("magnify: a warp goes at the FRONT of the chain") {
    // For grab's reason: the region weight has to be read at the assembled
    // point, not at one an existing deformer has already moved.
    Document doc = two_balls();
    Node* first = doc.layers[0].sdf->find_mut(doc.layers[0].sdf->roots[0]);
    first->deformers.push_back(Deformer::twist(1.0f));

    MagnifySettings s;
    s.radius = 0.8f;
    apply_warps(doc, 1, brush::magnify_brush(doc.layers[0], cf3(0, 0, 0), 0.4f, s));

    const Node* after = doc.layers[0].sdf->find(doc.layers[0].sdf->roots[0]);
    REQUIRE(after->deformers.size() == 2);
    CHECK(after->deformers[0].type == kernel::cdeform_magnify);
    CHECK(after->deformers[1].type == kernel::cdeform_twist);
}

TEST_CASE("magnify: a gesture coalesces instead of stacking one warp per frame") {
    // A live pinch calls this every frame with a growing strength. Without the
    // replace rule the chain grows without bound and the declared Lipschitz
    // compounds with it.
    Document doc = two_balls();
    MagnifySettings s;
    s.radius = 0.8f;
    for (float strength : {0.1f, 0.2f, 0.3f, 0.4f})
        apply_warps(doc, 1, brush::magnify_brush(doc.layers[0], cf3(0, 0, 0), strength, s));

    for (NodeId id : doc.layers[0].sdf->roots) {
        const Node* n = doc.layers[0].sdf->find(id);
        REQUIRE(n->deformers.size() == 1);
        CHECK(n->deformers[0].ext[0] == doctest::Approx(0.4f));  // the last frame, not the sum
    }

    // ...and the result is the same document a single frame at the final
    // strength would have produced.
    Document once = two_balls();
    apply_warps(once, 1, brush::magnify_brush(once.layers[0], cf3(0, 0, 0), 0.4f, s));
    Tape a = compile_document(doc);
    Tape b = compile_document(once);
    for (float x = -1.2f; x <= 1.2f; x += 0.05f)
        for (float y = -1.0f; y <= 1.0f; y += 0.05f)
            CHECK(a.eval(cf3(x, y, 0)).d == b.eval(cf3(x, y, 0)).d);
}

TEST_CASE("magnify: a pinch does not swallow a drag's grab over the same ball") {
    // The chain rule matches by KIND as well as by ball, which is what keeps
    // two different gestures over one spot from folding into each other.
    Document doc = two_balls();
    brush::MoveSettings m;
    m.radius = 0.8f;
    apply_warps(doc, 1, brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0.3f, 0), m));

    MagnifySettings s;
    s.radius = 0.8f;
    apply_warps(doc, 1, brush::magnify_brush(doc.layers[0], cf3(0, 0, 0), 0.4f, s));

    const Node* n = doc.layers[0].sdf->find(doc.layers[0].sdf->roots[0]);
    REQUIRE(n->deformers.size() == 2);
    CHECK(n->deformers[0].type == kernel::cdeform_magnify);
    CHECK(n->deformers[1].type == kernel::cdeform_grab);
}

TEST_CASE("magnify: a prepared gesture reproduces magnify_brush exactly, frame by frame") {
    // The live path and the one-shot path must be the same arithmetic, or a
    // preview is a preview of something else. Bit-for-bit, not approximately.
    Document doc = two_balls();
    MagnifySettings s;
    s.radius = 0.8f;
    const std::vector<brush::PreparedMove> prepared =
        brush::prepare_magnify(doc.layers[0], cf3(0.1f, 0.05f, 0), s);

    for (float strength : {-0.6f, -0.2f, 0.15f, 0.5f}) {
        const std::vector<MoveWarp> direct =
            brush::magnify_brush(doc.layers[0], cf3(0.1f, 0.05f, 0), strength, s);
        REQUIRE(direct.size() == prepared.size());
        MoveWarp reused;
        for (std::size_t i = 0; i < prepared.size(); ++i) {
            brush::resolve_prepared_magnify(prepared[i], strength, &reused);
            CHECK(reused.node == direct[i].node);
            REQUIRE(reused.deformers.size() == direct[i].deformers.size());
            for (std::size_t k = 0; k < reused.deformers.size(); ++k) {
                const Deformer& x = reused.deformers[k];
                const Deformer& y = direct[i].deformers[k];
                CHECK(x.type == y.type);
                CHECK(x.k == y.k);
                CHECK(x.a == y.a);
                CHECK(x.b == y.b);
                CHECK(x.c == y.c);
                CHECK(x.ext[0] == y.ext[0]);
            }
        }
    }
}

TEST_CASE("magnify: preparing walks the tree, resolving does not") {
    Document doc = two_balls();
    MagnifySettings s;
    s.radius = 0.8f;
    brush::MovePrepareStats stats;
    const std::vector<brush::PreparedMove> prepared =
        brush::prepare_magnify(doc.layers[0], cf3(0, 0, 0), s, &stats);
    CHECK(stats.visited == 2);
    CHECK(stats.reached == 2);
    CHECK(prepared.size() == 2);
}

// -- symmetry -----------------------------------------------------------------

TEST_CASE("magnify: a mirrored gesture is the mirror image of its mirror image") {
    // Bit for bit on an identity layer transform, which is what makes a pinch
    // made from the left of a symmetric model the same sculpt as one made from
    // the right. The strength is the same for every image — a reflection of a
    // radial scale is a radial scale — so this has one fewer way to go wrong
    // than the drag it is modelled on, and must not have gained a new one.
    const cfloat3 at = cf3(0.6f, 0.2f, 0.1f);
    const cfloat3 mirrored = cf3(-0.6f, 0.2f, 0.1f);

    auto build = [](cfloat3 centre) {
        Document doc = two_balls(0.3f);
        doc.layers[0].mirror_axes = 1u;  // about x
        MagnifySettings s;
        s.radius = 0.7f;
        apply_warps(doc, 1, brush::magnify_brush(doc.layers[0], centre, -0.35f, s));
        return compile_document(doc);
    };
    Tape from_right = build(at);
    Tape from_left = build(mirrored);

    for (float x = -1.4f; x <= 1.4f; x += 0.05f)
        for (float y = -1.0f; y <= 1.0f; y += 0.05f)
            CHECK(from_right.eval(cf3(x, y, 0.1f)).d == from_left.eval(cf3(-x, y, 0.1f)).d);
}

TEST_CASE("magnify: material under the ball is reached even when it is a copy") {
    // Selection is on the item's OWN bound against the gesture's images, so an
    // item whose mirror copy sits under the cursor is reached through that
    // copy. Without that, pinching a mirrored ear does nothing at all on the
    // side you are looking at.
    Document doc;
    Layer layer;
    layer.id = 1;
    layer.kind = LayerKind::Sdf;
    layer.sdf = std::make_shared<SdfContent>();
    layer.mirror_axes = 1u;
    Node ball;
    ball.prim = Prim::sphere(0.4f);
    ball.xform.position = cf3(0.9f, 0, 0);  // its copy sits at -0.9
    layer.sdf->insert(ball);
    doc.layers.push_back(layer);

    MagnifySettings s;
    s.radius = 0.5f;
    // Centred on the COPY, nowhere near the item itself.
    const std::vector<MoveWarp> warps =
        brush::magnify_brush(doc.layers[0], cf3(-0.9f, 0, 0), 0.4f, s);
    REQUIRE(warps.size() == 1);
    CHECK(warps[0].deformers.size() == 1);
    // Resolved through the REFLECTED image, whose centre is +0.9 — which in
    // the item's own frame, the item sitting at +0.9, is its centre. Resolved
    // through the gesture itself it would have been -1.8, half a document away
    // from anything the item occupies.
    CHECK(warps[0].deformers[0].k == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("magnify: a straddler's deformers are ordered by their values") {
    // Two images reach an item straddling the plane, and the order they end up
    // in must not depend on which side the gesture was made from — once the
    // item is not itself plane-symmetric those are two different fields.
    Document doc;
    Layer layer;
    layer.id = 1;
    layer.kind = LayerKind::Sdf;
    layer.sdf = std::make_shared<SdfContent>();
    layer.mirror_axes = 1u;
    Node bar;
    bar.prim = Prim::box(cf3(1.2f, 0.3f, 0.3f));  // spans the plane
    layer.sdf->insert(bar);
    doc.layers.push_back(layer);

    MagnifySettings s;
    s.radius = 0.9f;
    const std::vector<MoveWarp> right =
        brush::magnify_brush(doc.layers[0], cf3(0.5f, 0, 0), -0.3f, s);
    const std::vector<MoveWarp> left =
        brush::magnify_brush(doc.layers[0], cf3(-0.5f, 0, 0), -0.3f, s);
    REQUIRE(right.size() == 1);
    REQUIRE(left.size() == 1);
    REQUIRE(right[0].deformers.size() == 2);
    REQUIRE(left[0].deformers.size() == 2);
    // Same two deformations, same order, whichever side asked for them.
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(right[0].deformers[i].k == left[0].deformers[i].k);
        CHECK(right[0].deformers[i].ext[0] == left[0].deformers[i].ext[0]);
    }
}
