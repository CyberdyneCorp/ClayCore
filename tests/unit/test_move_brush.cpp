// The Move brush (brush-engine and scene-model specs, add-move-brush): dragging
// the assembled surface rather than one item of it, and the command that makes
// it something you can apply to an existing sculpt at all.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/brush/move.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using brush::MoveSettings;
using brush::MoveWarp;
using kernel::cf3;
using kernel::cfloat3;
using namespace clay::scene;

namespace {

// Two balls smooth-unioned into one form: the case a naive Move gets wrong,
// because the region under the cursor belongs to more than one item.
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

// Apply the warps the way a host would: one SetDeformersCmd per node, each
// putting the move at the front of that node's chain.
std::size_t apply_move(Document& doc, LayerId layer, const std::vector<MoveWarp>& warps) {
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

// -- the thing a grab on one item cannot do -----------------------------------

TEST_CASE("move: a blended form moves as one surface") {
    const Document before = two_balls();
    const float base_left = surface_y(before, -0.45f);
    const float base_right = surface_y(before, 0.45f);
    const float base_centre = surface_y(before, 0.0f);

    Document doc = two_balls();
    MoveSettings s;
    s.radius = 0.8f;
    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0), s);
    REQUIRE(warps.size() == 2);  // both items take a share
    CHECK(apply_move(doc, 1, warps) == 2);

    const float left = surface_y(doc, -0.45f) - base_left;
    const float right = surface_y(doc, 0.45f) - base_right;
    const float centre = surface_y(doc, 0.0f) - base_centre;

    CHECK(left > 0.0f);
    CHECK(right > 0.0f);
    CHECK(left == doctest::Approx(right).epsilon(0.1));  // symmetric about the drag
    CHECK(centre >= std::max(left, right));              // and peaks at its centre
}

TEST_CASE("move: a grab on one item is NOT the same thing") {
    // The whole reason this resolver exists. Same deformation, put on one item
    // by hand: its side moves and the other is left behind.
    const Document before = two_balls();
    Document doc = two_balls();
    Node* left = doc.layers[0].sdf->find_mut(doc.layers[0].sdf->roots[0]);
    left->deformers.push_back(
        Deformer::grab(cf3(0, 0, 0), 0.8f, cf3(0, 0.4f, 0)));

    const float moved_left = surface_y(doc, -0.45f) - surface_y(before, -0.45f);
    const float moved_right = surface_y(doc, 0.45f) - surface_y(before, 0.45f);
    CHECK(moved_left > 0.02f);
    CHECK(moved_right == doctest::Approx(0.0f).epsilon(0.05));
}

TEST_CASE("move: the centre is in WORLD space, not the item's") {
    // A grab's own centre is local, which is the trap. The resolver takes world
    // coordinates, so a drag aimed at an item lands on it wherever it sits.
    Document doc;
    Layer layer;
    layer.id = 1;
    layer.kind = LayerKind::Sdf;
    layer.sdf = std::make_shared<SdfContent>();
    Node ball;
    ball.prim = Prim::sphere(0.5f);
    ball.op = Op::Add;
    ball.xform.position = cf3(1.5f, 0, 0);
    layer.sdf->insert(ball);
    doc.layers.push_back(layer);

    const float before = surface_y(doc, 1.5f);
    MoveSettings s;
    s.radius = 0.8f;
    // Aimed at the item's WORLD position.
    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(1.5f, 0, 0), cf3(0, 0.4f, 0), s);
    REQUIRE(warps.size() == 1);
    apply_move(doc, 1, warps);
    CHECK(surface_y(doc, 1.5f) > before + 0.02f);

    // ...and one aimed at the origin, where the item is not, does nothing to it.
    Document elsewhere;
    elsewhere.layers.push_back(layer);
    const std::vector<MoveWarp> miss =
        brush::move_brush(elsewhere.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0), s);
    CHECK(miss.empty());  // culled: the drag cannot reach it
}

TEST_CASE("move: a layer transform is mapped through") {
    // Built twice rather than copied: Layer holds its content by shared_ptr
    // ("shared between instances"), so a copied Document is not a snapshot.
    const auto placed = [] {
        Document d = two_balls();
        d.layers[0].xform.position = cf3(0, 0, 3.0f);
        d.layers[0].xform.scale = 2.0f;
        return d;
    };
    const Document before = placed();
    Document doc = placed();
    MoveSettings s;
    s.radius = 1.6f;  // world units, so it must survive the layer's scale
    // The form now sits at world z = 3, scaled by two.
    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(0, 0, 3.0f), cf3(0, 0.5f, 0), s);
    REQUIRE(warps.size() == 2);
    apply_move(doc, 1, warps);

    const auto surface_y_at_z = [](const Document& d, float z) {
        Tape t = compile_document(d);
        float last = 1.0f;
        for (float y = 4.0f; y > -4.0f; y -= 0.004f) {
            const float v = t.eval(cf3(0, y, z)).d;
            if (v <= 0.0f && last > 0.0f) return y;
            last = v;
        }
        return 0.0f;
    };
    CHECK(surface_y_at_z(doc, 3.0f) > surface_y_at_z(before, 3.0f) + 0.05f);
}

TEST_CASE("move: items out of reach take no warp") {
    Document doc = two_balls();
    Node far;
    far.prim = Prim::sphere(0.3f);
    far.op = Op::Add;
    far.xform.position = cf3(50.0f, 0, 0);
    doc.layers[0].sdf->insert(far);

    MoveSettings s;
    s.radius = 0.8f;
    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0), s);
    // The two near balls, and not the far one: a no-op deformer still costs a
    // tape record on every evaluation.
    CHECK(warps.size() == 2);
}

TEST_CASE("move: a nested item still moves") {
    Document doc;
    Layer layer;
    layer.id = 1;
    layer.kind = LayerKind::Sdf;
    layer.sdf = std::make_shared<SdfContent>();
    Node group;
    group.is_group = true;
    group.op = Op::Add;
    const NodeId gid = layer.sdf->insert(group);
    Node ball;
    ball.prim = Prim::sphere(0.5f);
    ball.op = Op::Add;
    layer.sdf->insert(ball, gid);
    doc.layers.push_back(layer);

    const float before = surface_y(doc, 0.0f);
    MoveSettings s;
    s.radius = 0.8f;
    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0), s);
    // The child, not the group: a group's transform does not reach its children
    // here, so it is the children that carry a drag.
    REQUIRE(warps.size() == 1);
    CHECK(warps[0].node != gid);
    apply_move(doc, 1, warps);
    CHECK(surface_y(doc, 0.0f) > before + 0.02f);
}

TEST_CASE("move: refusals produce nothing") {
    Document doc = two_balls();
    MoveSettings s;
    s.radius = 0.0f;
    CHECK(brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0), s).empty());
    s.radius = -1.0f;
    CHECK(brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0), s).empty());
    s.radius = 0.8f;
    // A drag of nowhere is not a drag.
    CHECK(brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0, 0), s).empty());
}

TEST_CASE("move: the pull is monotonic, and short of what was asked") {
    const float base = surface_y(two_balls(), 0.0f);
    float previous = 0.0f;
    for (float d : {0.1f, 0.2f, 0.4f}) {
        Document doc = two_balls();
        MoveSettings s;
        s.radius = 0.8f;
        apply_move(doc, 1, brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, d, 0), s));
        const float lift = surface_y(doc, 0.0f) - base;
        CHECK(lift > previous);  // further every time...
        CHECK(lift < d);         // ...but never the whole displacement: grab
                                 // weights at the sample, not at its preimage
        previous = lift;
    }
}

// -- the ordering rule --------------------------------------------------------

TEST_CASE("move: a warp goes at the FRONT of the chain") {
    // deformers[0] warps the point first and is therefore the outermost warp on
    // the geometry. Appended instead, the grab's region weight is read at a
    // point the existing deformer already moved — so the drag acts somewhere
    // other than where it was aimed.
    Node node;
    node.prim = Prim::sphere(0.5f);
    node.deformers.push_back(Deformer::twist(1.5f));

    MoveWarp warp;
    warp.deformer = Deformer::grab(cf3(0, 0, 0), 0.8f, cf3(0, 0.4f, 0));
    const std::vector<Deformer> chain = brush::moved_chain(node, warp);

    REQUIRE(chain.size() == 2);
    CHECK(chain[0].type == kernel::cdeform_grab);   // the move is outermost
    CHECK(chain[1].type == kernel::cdeform_twist);  // what was there follows
}

TEST_CASE("move: position in the chain changes the field") {
    // Not a preference — the two orders are different deformations, which is
    // why the rule above has to be owned somewhere.
    const auto build = [](bool move_first) {
        Document doc;
        Layer layer;
        layer.id = 1;
        layer.kind = LayerKind::Sdf;
        layer.sdf = std::make_shared<SdfContent>();
        Node n;
        n.prim = Prim::box(cf3(0.5f, 0.3f, 0.4f));
        n.op = Op::Add;
        const Deformer grab = Deformer::grab(cf3(0.35f, 0, 0), 0.5f, cf3(0, 0.3f, 0));
        const Deformer magnify = Deformer::magnify(cf3(0.35f, 0, 0), 0.4f, 0.6f);
        if (move_first) {
            n.deformers = {grab, magnify};
        } else {
            n.deformers = {magnify, grab};
        }
        layer.sdf->insert(n);
        doc.layers.push_back(layer);
        return doc;
    };

    Tape first = compile_document(build(true));
    Tape last = compile_document(build(false));
    float worst = 0.0f;
    for (float x = -0.8f; x <= 0.8f; x += 0.05f)
        for (float y = -0.8f; y <= 0.8f; y += 0.05f)
            worst = std::max(worst, std::abs(first.eval(cf3(x, y, 0)).d -
                                             last.eval(cf3(x, y, 0)).d));
    CHECK(worst > 1e-3f);
}

// -- the command --------------------------------------------------------------

TEST_CASE("move: SetDeformersCmd replaces a chain and undoes exactly") {
    Document doc = two_balls();
    const NodeId id = doc.layers[0].sdf->roots[0];
    Tape before = compile_document(doc);

    SetDeformersCmd cmd{1, id, {Deformer::grab(cf3(0, 0, 0), 0.8f, cf3(0, 0.4f, 0))}};
    std::optional<Command> inverse = scene::apply(doc, Command{cmd});
    REQUIRE(inverse.has_value());
    CHECK(compile_document(doc).eval(cf3(0, 0.55f, 0)).d !=
          doctest::Approx(before.eval(cf3(0, 0.55f, 0)).d));

    REQUIRE(scene::apply(doc, *inverse).has_value());
    Tape after = compile_document(doc);
    for (float y = -1.0f; y <= 1.0f; y += 0.05f)
        CHECK(after.eval(cf3(0, y, 0)).d == doctest::Approx(before.eval(cf3(0, y, 0)).d));
}

TEST_CASE("move: SetDeformersCmd refuses a node that is not there") {
    Document doc = two_balls();
    CHECK_FALSE(scene::apply(doc, Command{SetDeformersCmd{1, 9999, {}}}).has_value());
    CHECK_FALSE(scene::apply(doc, Command{SetDeformersCmd{77, doc.layers[0].sdf->roots[0], {}}})
                    .has_value());
}

TEST_CASE("move: a whole drag is one undo step") {
    Document doc = two_balls();
    UndoStack undo;
    Tape before = compile_document(doc);

    undo.begin_group();
    const std::vector<MoveWarp> warps = brush::move_brush(
        doc.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0), MoveSettings{0.8f, 0, false});
    REQUIRE(warps.size() == 2);
    for (const MoveWarp& w : warps) {
        const Node* n = doc.layers[0].sdf->find(w.node);
        undo.perform(doc, Command{SetDeformersCmd{1, w.node, brush::moved_chain(*n, w)}});
    }
    undo.end_group();

    CHECK(undo.undo_depth() == 1);  // one gesture, one step
    REQUIRE(undo.undo(doc));
    Tape after = compile_document(doc);
    for (float y = -1.0f; y <= 1.0f; y += 0.05f)
        CHECK(after.eval(cf3(0, y, 0)).d == doctest::Approx(before.eval(cf3(0, y, 0)).d));
}

// -- a live drag ---------------------------------------------------------------

TEST_CASE("move: a drag coalesces instead of stacking one warp per frame") {
    // A drag holds its centre and radius fixed and only grows the displacement.
    // Appending per frame would grow the chain without bound and compound the
    // declared Lipschitz with every frame of it, so the leading warp of the
    // same drag is REPLACED.
    Document stepped = two_balls();
    for (float d : {0.08f, 0.16f, 0.24f, 0.32f, 0.4f}) {
        const std::vector<MoveWarp> warps =
            brush::move_brush(stepped.layers.front(), cf3(0, 0, 0), cf3(0, d, 0),
                              MoveSettings{1.2f, 0, false});
        REQUIRE(warps.size() == 2);
        REQUIRE(apply_move(stepped, 1, warps) == 2);
    }
    for (NodeId id : stepped.layers.front().sdf->roots) {
        CAPTURE(id);
        CHECK(stepped.layers.front().sdf->find(id)->deformers.size() == 1);
    }

    // ...and five frames ending at 0.4 are the same field as one drag of 0.4,
    // which is the property that actually matters: it catches a stacking bug
    // whether or not the chain length happens to look right.
    Document once = two_balls();
    REQUIRE(apply_move(once, 1,
                       brush::move_brush(once.layers.front(), cf3(0, 0, 0), cf3(0, 0.4f, 0),
                                         MoveSettings{1.2f, 0, false})) == 2);
    Tape a = compile_document(stepped);
    Tape b = compile_document(once);
    for (float x = -1.2f; x <= 1.2f; x += 0.053f)
        for (float y = -1.0f; y <= 1.2f; y += 0.061f) {
            const cfloat3 p = cf3(x, y, 0.07f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(a.eval(p).d == doctest::Approx(b.eval(p).d));
        }
    CHECK(kernel::csafe_step_scale(a.info) == doctest::Approx(kernel::csafe_step_scale(b.info)));
}

TEST_CASE("move: a different drag is kept beside the first, not folded into it") {
    Document doc = two_balls();
    REQUIRE(apply_move(doc, 1,
                       brush::move_brush(doc.layers.front(), cf3(0, 0, 0), cf3(0, 0.3f, 0),
                                         MoveSettings{1.2f, 0, false})) == 2);
    // A different centre is a different gesture: it must not replace the first.
    const std::vector<MoveWarp> second =
        brush::move_brush(doc.layers.front(), cf3(0.4f, 0.2f, 0), cf3(0.2f, 0, 0),
                          MoveSettings{0.9f, 0, false});
    REQUIRE_FALSE(second.empty());
    const Node* n = doc.layers.front().sdf->find(second.front().node);
    REQUIRE(n != nullptr);
    CHECK(brush::moved_chain(*n, second.front()).size() == n->deformers.size() + 1);
}

TEST_CASE("move: a drag on a chain it does not own leaves that chain alone") {
    // Coalescing keys on the leading warp being the SAME drag. An item carrying
    // an unrelated deformer must keep it, and the move must still go in front.
    Document doc = two_balls();
    Node* first = doc.layers.front().sdf->find_mut(doc.layers.front().sdf->roots.front());
    REQUIRE(first != nullptr);
    first->deformers.push_back(Deformer::twist(0.9f));

    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers.front(), cf3(0, 0, 0), cf3(0, 0.3f, 0),
                          MoveSettings{1.2f, 0, false});
    REQUIRE_FALSE(warps.empty());
    const std::vector<Deformer> chain = brush::moved_chain(*first, warps.front());
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].type == kernel::cdeform_grab);   // the move, outermost
    CHECK(chain[1].type == kernel::cdeform_twist);  // what was already there
}

TEST_CASE("move: a moved document still loads at every earlier scene minor") {
    // SetDeformersCmd took kSceneMinor to 5. The NODE encoding is untouched, so
    // every earlier minor must still read — and a document carrying a deformer
    // chain is the case that would break first if it ever did.
    //
    // Read at the minor it was WRITTEN at, which is what the container does:
    // the scene payload carries no version of its own, and .clayspace passes
    // the one from its own header down.
    Document doc = two_balls();
    REQUIRE(apply_move(doc, 1,
                       brush::move_brush(doc.layers.front(), cf3(0, 0, 0), cf3(0, 0.3f, 0),
                                         MoveSettings{1.2f, 0, false})) == 2);
    const float reference = compile_document(doc).eval(cf3(0, 0.5f, 0)).d;

    for (std::uint16_t minor : {std::uint16_t(2), std::uint16_t(3), std::uint16_t(4),
                                kSceneMinor}) {
        CAPTURE(minor);
        const std::vector<std::uint8_t> bytes = serialize_document(doc, minor);
        auto back = deserialize_document(bytes.data(), bytes.size(), minor);
        REQUIRE(back.has_value());
        CHECK(compile_document(*back).eval(cf3(0, 0.5f, 0)).d == doctest::Approx(reference));
    }
}

// -- a squashed item (#320) ---------------------------------------------------
// The per-axis scale is applied INNERMOST and the tape takes it off before the
// deformer chain runs, so a warp authored in the item's PLACED frame lands
// where the squashed item is not. `layer.xform * node.xform` composed the
// placed frame and dropped the scale, so a drag on the surface of a stretched
// item did nothing whatsoever.

namespace {

// One sphere, optionally scaled per axis. Its local geometry is identical in
// every case, so anything the drag does differently is the FRAME.
Document one_ball(cfloat3 axes) {
    Document doc;
    Layer layer;
    layer.id = 1;
    layer.kind = LayerKind::Sdf;
    layer.sdf = std::make_shared<SdfContent>();
    Node ball;
    ball.prim = Prim::sphere(1.0f);
    ball.op = Op::Add;
    ball.scale_axes = axes;
    layer.sdf->insert(ball);
    doc.layers.push_back(layer);
    return doc;
}

float value_at(const Document& doc, cfloat3 p) { return compile_document(doc).eval(p).d; }

}  // namespace

TEST_CASE("move: a drag reaches the surface of a STRETCHED item") {
    // Stretched 3x on X, so its surface is at world x = 3. On the placed frame
    // alone that point maps to local (3, 0, 0) — far outside the unit sphere —
    // and the grab's falloff reached nothing at all.
    Document doc = one_ball(cf3(3.0f, 1.0f, 1.0f));
    const cfloat3 grab = cf3(3.0f, 0, 0);
    REQUIRE(value_at(doc, grab) == doctest::Approx(0.0f).epsilon(1e-3));

    MoveSettings s;
    s.radius = 0.6f;
    const std::vector<MoveWarp> warps = brush::move_brush(doc.layers[0], grab, cf3(0, 0.4f, 0), s);
    REQUIRE(warps.size() == 1);
    REQUIRE(apply_move(doc, 1, warps) == 1);
    CHECK(value_at(doc, grab) > 0.05f);  // the surface moved off the point
}

TEST_CASE("move: a stretched item drags exactly as the unstretched one does") {
    // The strongest form of the same statement. The local geometry is the same
    // sphere either way, and the grab is authored in local space, so a drag on
    // the corresponding point must produce the SAME local warp — the frame is
    // the only thing that differs, and mapping it correctly is what makes the
    // two agree.
    Document plain = one_ball(cf3(1.0f, 1.0f, 1.0f));
    Document wide = one_ball(cf3(3.0f, 1.0f, 1.0f));
    MoveSettings s;
    s.radius = 0.6f;

    const std::vector<MoveWarp> a =
        brush::move_brush(plain.layers[0], cf3(1.0f, 0, 0), cf3(0, 0.4f, 0), s);
    const std::vector<MoveWarp> b =
        brush::move_brush(wide.layers[0], cf3(3.0f, 0, 0), cf3(0, 0.4f, 0), s);
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);

    // The grab centre is the same local point in both.
    CHECK(b[0].deformer.k == doctest::Approx(a[0].deformer.k));
    CHECK(b[0].deformer.a == doctest::Approx(a[0].deformer.a));
    CHECK(b[0].deformer.b == doctest::Approx(a[0].deformer.b));

    REQUIRE(apply_move(plain, 1, a) == 1);
    REQUIRE(apply_move(wide, 1, b) == 1);
    CHECK(value_at(wide, cf3(3.0f, 0, 0)) ==
          doctest::Approx(value_at(plain, cf3(1.0f, 0, 0))).epsilon(1e-3));
}

TEST_CASE("move: a squashed frame never drags outside what was circled") {
    // A grab carries ONE radius and a squashed frame turns the world sphere
    // into a local ellipsoid, so no scalar is exact. The choice is the
    // conservative one — divide by the LARGEST factor — so every world reach is
    // at most the radius the artist enclosed. Pinned as a direction, since the
    // opposite choice is equally arithmetic and takes geometry nobody circled.
    Document doc = one_ball(cf3(4.0f, 1.0f, 1.0f));
    MoveSettings s;
    s.radius = 0.5f;
    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(4.0f, 0, 0), cf3(0, 0.3f, 0), s);
    REQUIRE(warps.size() == 1);
    // c is the grab's local radius; the widest world reach it can have is
    // c * max(axes), which must not exceed the radius circled.
    CHECK(warps[0].deformer.c * 4.0f <= doctest::Approx(0.5f));
    REQUIRE(apply_move(doc, 1, warps) == 1);
    // Well outside the circle along the stretched axis, the surface is where it
    // always was.
    Document untouched = one_ball(cf3(4.0f, 1.0f, 1.0f));
    const cfloat3 far_off = cf3(0.0f, 1.0f, 0.0f);
    CHECK(value_at(doc, far_off) == doctest::Approx(value_at(untouched, far_off)).epsilon(1e-4));
}
