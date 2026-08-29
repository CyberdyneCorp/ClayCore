// The Move brush (brush-engine and scene-model specs, add-move-brush): dragging
// the assembled surface rather than one item of it, and the command that makes
// it something you can apply to an existing sculpt at all.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "clay/brush/move.h"
#include "clay/field/volume.h"
#include "clay/math/transform.h"
#include "clay/scene/bounds.h"
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
    warp.deformers.push_back(Deformer::grab(cf3(0, 0, 0), 0.8f, cf3(0, 0.4f, 0)));
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
    REQUIRE(a[0].deformers.size() == 1);
    REQUIRE(b[0].deformers.size() == 1);
    CHECK(b[0].deformers[0].k == doctest::Approx(a[0].deformers[0].k));
    CHECK(b[0].deformers[0].a == doctest::Approx(a[0].deformers[0].a));
    CHECK(b[0].deformers[0].b == doctest::Approx(a[0].deformers[0].b));

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
    REQUIRE(warps[0].deformers.size() == 1);
    CHECK(warps[0].deformers[0].c * 4.0f <= doctest::Approx(0.5f));
    REQUIRE(apply_move(doc, 1, warps) == 1);
    // Well outside the circle along the stretched axis, the surface is where it
    // always was.
    Document untouched = one_ball(cf3(4.0f, 1.0f, 1.0f));
    const cfloat3 far_off = cf3(0.0f, 1.0f, 0.0f);
    CHECK(value_at(doc, far_off) == doctest::Approx(value_at(untouched, far_off)).epsilon(1e-4));
}

// -- prepare / resolve (sdf-sculpt-transaction spec) ---------------------------
//
// A drag holds its anchor and radius fixed and only grows the displacement, so
// which items it reaches and where its centre lands in each of their frames is
// decided once. `move_brush` is now that preparation followed by that
// resolution, which is the point: there is ONE resolver, so a live drag cannot
// drift away from what committing through the old entry point would produce.

namespace {

bool same_deformer(const Deformer& a, const Deformer& b) {
    if (a.type != b.type || a.ease != b.ease) return false;
    if (a.k != b.k || a.a != b.a || a.b != b.b || a.c != b.c) return false;
    for (int i = 0; i < 6; ++i)
        if (a.ext[i] != b.ext[i]) return false;
    return true;
}

// Every warp `move_brush` produces, reproduced from a preparation taken once.
// BIT-identical, not close: a preview and its commit must be the same field.
void check_prepare_matches(const Layer& layer, cfloat3 centre, cfloat3 displacement,
                           const MoveSettings& settings) {
    const std::vector<MoveWarp> direct = brush::move_brush(layer, centre, displacement, settings);
    const std::vector<brush::PreparedMove> prepared =
        brush::prepare_move(layer, centre, settings);
    REQUIRE(prepared.size() == direct.size());
    for (std::size_t i = 0; i < direct.size(); ++i) {
        const MoveWarp resolved = brush::resolve_prepared_move(prepared[i], displacement);
        CHECK(resolved.node == direct[i].node);
        REQUIRE(resolved.deformers.size() == direct[i].deformers.size());
        for (std::size_t g = 0; g < resolved.deformers.size(); ++g)
            CHECK(same_deformer(resolved.deformers[g], direct[i].deformers[g]));
        // ...and the rest of the gesture's identity, which is what a later
        // frame's `moved_chain` recognises its earlier frames by.
        REQUIRE(resolved.gesture.size() == direct[i].gesture.size());
        for (std::size_t g = 0; g < resolved.gesture.size(); ++g)
            CHECK(same_deformer(resolved.gesture[g], direct[i].gesture[g]));
    }
}

}  // namespace

// -- a drag under symmetry (#363) ---------------------------------------------
// The compiler emits a mirrored item as itself plus a copy per axis, and a
// copy's field at p is the item's whole record — deformer chain included — at
// the reflected point. A grab in an item's chain therefore moves the item near
// the ball AND its copy near the ball's reflection; an item whose COPY sits
// under the ball has its body where that grab weighs zero, and nothing moves.
// Selecting on the mirror-expanded bound made every item on both sides a
// candidate and warped the far ones for nothing. The brush is reflected
// instead: every item is tested on its OWN bound against the ball and each
// image the layer's symmetry makes of it, and takes one grab per image that
// reaches it, at that image's centre with that image's displacement.
//
// Every property below was verified to FAIL by reverting the one guard that
// makes it true; each case says which.

namespace {

// One SDF layer, x-mirrored at seam .05 when asked, identity transform.
Document symmetric_layer(bool mirror_on) {
    Document doc;
    Layer layer;
    layer.id = 1;
    layer.kind = LayerKind::Sdf;
    layer.sdf = std::make_shared<SdfContent>();
    if (mirror_on) {
        layer.mirror_axes = kMirrorX;
        layer.mirror_k = 0.05f;
    }
    doc.layers.push_back(layer);
    return doc;
}

NodeId add_ball(Document& doc, cfloat3 pos, float r, bool mirror = true, float k = 0.05f) {
    Node n;
    n.prim = Prim::sphere(r);
    n.op = Op::Add;
    n.blend = k > 0.0f ? Blend{BlendProfile::Quadratic, k} : Blend{BlendProfile::Hard, 0.0f};
    n.xform.position = pos;
    n.mirror = mirror;
    return doc.layers[0].sdf->insert(n);
}

// The oracle fixture: base r .4 at the origin (straddles the plane); A on +x;
// B on -x, placed so that its COPY (1, -.3, 0) sits under the ball at W; S a
// straddler out of this drag's reach; C, when asked for, an OPTED-OUT ball
// that only the reflected ball would reach.
struct Oracle {
    Document doc;
    NodeId base = kNoNode, A = kNoNode, B = kNoNode, S = kNoNode, C = kNoNode;
};

Oracle oracle(bool mirror_on, bool with_C = false) {
    Oracle o;
    o.doc = symmetric_layer(mirror_on);
    o.base = add_ball(o.doc, cf3(0, 0, 0), 0.4f);
    o.A = add_ball(o.doc, cf3(1.0f, 0.3f, 0), 0.2f);
    o.B = add_ball(o.doc, cf3(-1.0f, -0.3f, 0), 0.2f);
    o.S = add_ball(o.doc, cf3(0, 1.5f, 0), 0.2f);
    if (with_C) o.C = add_ball(o.doc, cf3(-1.0f, 0, 0.5f), 0.15f, /*mirror=*/false);
    return o;
}

const cfloat3 kW = cf3(1.1f, 0, 0);    // the drag
const cfloat3 kD = cf3(0.2f, 0, 0);
const cfloat3 kRW = cf3(-1.1f, 0, 0);  // its mirror image
const cfloat3 kRD = cf3(-0.2f, 0, 0);
const MoveSettings kSettings{0.45f, 0, false};

std::vector<NodeId> nodes_of(const std::vector<MoveWarp>& warps) {
    std::vector<NodeId> ids;
    for (const MoveWarp& w : warps) ids.push_back(w.node);
    std::sort(ids.begin(), ids.end());
    return ids;
}

const MoveWarp* warp_on(const std::vector<MoveWarp>& warps, NodeId id) {
    for (const MoveWarp& w : warps)
        if (w.node == id) return &w;
    return nullptr;
}

// Field for field, never memcmp: Deformer carries padding.
bool same_grab(const Deformer& x, const Deformer& y) {
    if (x.type != y.type || x.ease != y.ease) return false;
    if (x.k != y.k || x.a != y.a || x.b != y.b || x.c != y.c) return false;
    for (int q = 0; q < 4; ++q)
        if (x.ext[q] != y.ext[q]) return false;
    return true;
}

// A lattice symmetric about x = 0, so the reflection of a sample is a sample:
// f(p) == f(Rp) can then be asked bit for bit. `origin` shifts it along y and
// z (never x) to sit over the item under test — a comparison over ground
// nothing moved passes whatever the rule.
struct Grid {
    int nx, ny, nz;
    std::vector<cfloat3> pts;
    Grid(int hx, int hy, int hz, float step, cfloat3 origin = cf3(0, 0, 0))
        : nx(2 * hx + 1), ny(2 * hy + 1), nz(2 * hz + 1) {
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    pts.push_back(origin + cf3(static_cast<float>(i - hx) * step,
                                               static_cast<float>(j - hy) * step,
                                               static_cast<float>(k - hz) * step));
    }
    std::size_t reflected_x(std::size_t idx) const {
        return (idx / static_cast<std::size_t>(nx)) * static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(nx - 1) - idx % static_cast<std::size_t>(nx);
    }
};

std::vector<float> sample(const Document& doc, const Grid& g) {
    Tape t = compile_document(doc);
    std::vector<float> out;
    out.reserve(g.pts.size());
    for (const cfloat3& p : g.pts) out.push_back(t.eval(p).d);
    return out;
}

std::size_t differing(const std::vector<float>& a, const std::vector<float>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}

std::size_t differing_from_reflection(const std::vector<float>& f, const Grid& g) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < f.size(); ++i) n += f[i] != f[g.reflected_x(i)];
    return n;
}

// Leftmost and rightmost surface crossing along x at height y.
std::pair<float, float> x_extent(const Document& doc, float y) {
    Tape t = compile_document(doc);
    float lo = 0.0f, hi = 0.0f;
    for (float x = -1.0f; x < 1.0f; x += 0.0005f)
        if (t.eval(cf3(x, y, 0)).d <= 0.0f) {
            lo = x;
            break;
        }
    for (float x = 1.0f; x > -1.0f; x -= 0.0005f)
        if (t.eval(cf3(x, y, 0)).d <= 0.0f) {
            hi = x;
            break;
        }
    return {lo, hi};
}

}  // namespace

TEST_CASE("move: a prepared drag reproduces move_brush exactly, frame by frame") {
    const cfloat3 centre = cf3(0, 0, 0);
    const MoveSettings settings{0.8f, 0, false};

    SUBCASE("a blended pair") {
        Document doc = two_balls();
        for (float d : {0.1f, -0.4f, 1.7f})
            check_prepare_matches(doc.layers[0], centre, cf3(0.3f, d, -0.2f), settings);
    }
    SUBCASE("a rotated item under a transformed layer") {
        Document doc = two_balls();
        doc.layers[0].xform.position = cf3(0.3f, -0.2f, 0.1f);
        doc.layers[0].xform.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), 0.7f);
        doc.layers[0].xform.scale = 1.4f;
        Node* n = doc.layers[0].sdf->find_mut(doc.layers[0].sdf->roots[0]);
        n->xform.rotation = math::Quat::from_axis_angle(cf3(1, 0, 0), -0.5f);
        n->xform.scale = 0.6f;
        check_prepare_matches(doc.layers[0], centre, cf3(0.2f, 0.5f, 0.1f), settings);
    }
    SUBCASE("a per-axis scale, which is innermost") {
        Document doc = two_balls();
        doc.layers[0].sdf->find_mut(doc.layers[0].sdf->roots[0])->scale_axes = cf3(3.0f, 1.0f, 0.5f);
        check_prepare_matches(doc.layers[0], centre, cf3(0.0f, 0.4f, 0.0f), settings);
    }
    SUBCASE("front_only and a non-default ease") {
        Document doc = two_balls();
        check_prepare_matches(doc.layers[0], centre, cf3(0, 0.4f, 0), MoveSettings{0.8f, 3, true});
    }
    SUBCASE("a group, whose children carry the drag") {
        Document doc = two_balls();
        Node group;
        group.is_group = true;
        const NodeId gid = doc.layers[0].sdf->insert(group);
        Node inner;
        inner.prim = Prim::sphere(0.3f);
        inner.xform.position = cf3(0, 0.4f, 0);
        doc.layers[0].sdf->insert(inner, gid);
        check_prepare_matches(doc.layers[0], centre, cf3(0, 0.4f, 0), settings);
    }
    SUBCASE("nothing in reach, and a radius that is not a drag") {
        Document doc = two_balls();
        check_prepare_matches(doc.layers[0], cf3(50, 0, 0), cf3(0, 0.4f, 0), MoveSettings{0.2f});
        CHECK(brush::prepare_move(doc.layers[0], centre, MoveSettings{0.0f}).empty());
    }
}

TEST_CASE("move: preparing walks the tree, resolving does not") {
    Document doc = two_balls();
    // Unrelated model, far out of reach of the drag.
    for (int i = 0; i < 2000; ++i) {
        Node dab;
        dab.prim = Prim::sphere(0.02f);
        dab.xform.position = cf3(40.0f + 0.1f * static_cast<float>(i), 0, 0);
        doc.layers[0].sdf->insert(dab);
    }

    brush::MovePrepareStats stats;
    const std::vector<brush::PreparedMove> prepared =
        brush::prepare_move(doc.layers[0], cf3(0, 0, 0), MoveSettings{0.8f, 0, false}, &stats);
    CHECK(stats.visited == 2002);  // the traversal that scales, paid ONCE
    CHECK(stats.reached == 2);
    CHECK(prepared.size() == 2);
    // ...and every frame after it costs the two items it moves. A counter, not
    // a clock: this must hold on a loaded CI machine as firmly as on an idle one.
    for (const brush::PreparedMove& p : prepared)
        CHECK(brush::resolve_prepared_move(p, cf3(0, 0.4f, 0)).node == p.node);
}

TEST_CASE("move: moved_chain against a chain is moved_chain against its node") {
    Document doc = two_balls();
    Node* n = doc.layers[0].sdf->find_mut(doc.layers[0].sdf->roots[0]);
    n->deformers.push_back(Deformer::twist(0.4f));

    const std::vector<MoveWarp> warps = brush::move_brush(doc.layers[0], cf3(0, 0, 0),
                                                          cf3(0, 0.4f, 0), MoveSettings{0.8f});
    REQUIRE(!warps.empty());
    const std::vector<Deformer> from_node = brush::moved_chain(*n, warps[0]);
    const std::vector<Deformer> from_chain = brush::moved_chain(n->deformers, warps[0]);
    REQUIRE(from_node.size() == from_chain.size());
    for (std::size_t i = 0; i < from_node.size(); ++i)
        CHECK(same_deformer(from_node[i], from_chain[i]));

    // The leading-grab replacement travels with it: a chain that already leads
    // with this drag's grab is continued, not stacked on.
    const std::vector<Deformer> again = brush::moved_chain(from_chain, warps[0]);
    CHECK(again.size() == from_chain.size());
}

TEST_CASE("move: under a mirror the drag selects what the ball or its reflection touches") {
    // Acceptance (5): the regression test for the selection itself. Selecting
    // on the mirror-expanded bound (item_influence_bound in place of the
    // item's own) takes {base, A, B} here, and B's grab has local centre
    // x = 2.1 — two diameters off B's body, a no-op. B is still selected
    // now, but through the REFLECTED ball: its grab is centred at local
    // x = -0.1 and pulls -x, which moves B's copy under the ball by +x.
    Oracle off = oracle(false);
    CHECK(nodes_of(brush::move_brush(off.doc.layers[0], kW, kD, kSettings)) ==
          std::vector<NodeId>{off.A});

    Oracle on = oracle(true);
    const std::vector<MoveWarp> warps = brush::move_brush(on.doc.layers[0], kW, kD, kSettings);
    CHECK(nodes_of(warps) == std::vector<NodeId>{on.A, on.B});

    const MoveWarp* b = warp_on(warps, on.B);
    REQUIRE(b != nullptr);
    REQUIRE(b->deformers.size() == 1);
    CHECK(b->deformers[0].k == doctest::Approx(-0.1f));
    CHECK(b->deformers[0].a == doctest::Approx(0.3f));
    CHECK(b->deformers[0].ext[0] == doctest::Approx(-0.2f));
    const MoveWarp* a = warp_on(warps, on.A);
    REQUIRE(a != nullptr);
    REQUIRE(a->deformers.size() == 1);
    CHECK(a->deformers[0].k == doctest::Approx(0.1f));
    CHECK(a->deformers[0].ext[0] == doctest::Approx(0.2f));
}

TEST_CASE("move: material under the ball moves even when it is a copy") {
    // The +x poles of A and of B's copy both sit under the ball; A's copy and
    // B's body are their reflections. All four move by the same amount. With
    // the grab aimed at the ball alone (image 0 only), the two B probes read
    // 0.00000 while the two A probes read -0.05945 — the ball moved half of
    // what was under it.
    const Oracle before = oracle(true);
    Oracle after = oracle(true);
    apply_move(after.doc, 1, brush::move_brush(after.doc.layers[0], kW, kD, kSettings));
    const cfloat3 probes[4] = {cf3(1.2f, 0.3f, 0), cf3(1.2f, -0.3f, 0), cf3(-1.2f, 0.3f, 0),
                               cf3(-1.2f, -0.3f, 0)};
    float deltas[4];
    for (int i = 0; i < 4; ++i)
        deltas[i] = value_at(after.doc, probes[i]) - value_at(before.doc, probes[i]);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(deltas[i] < -0.05f);
        CHECK(deltas[i] == doctest::Approx(deltas[0]).epsilon(1e-4));
    }
}

TEST_CASE("move: a mirrored drag is the mirror image of its mirror image, bit for bit") {
    // Acceptance (4). The +x drag on one document and its reflection on a
    // fresh one must leave identical chains and identical fields — which the
    // mirror-expanded selection did not (5,536 samples apart, by the whole
    // pull), because the +x drag moved A and the -x drag moved B. Identity
    // layer transform and no opted-out item, because a reflection is then an
    // exact sign flip and the +x drag's internal image IS the -x caller's
    // centre; a placed layer rounds through apply/apply_inverse (measured
    // <= 9e-8) and an opted-out item legitimately breaks the symmetry.
    // Reverting the displacement's reflection (image = reflected centre,
    // original pull) breaks this at every probe near B.
    const Grid g(26, 20, 12, 0.05f);
    Oracle plus = oracle(true), minus = oracle(true);
    const std::vector<float> undragged = sample(plus.doc, g);
    const std::vector<MoveWarp> wp = brush::move_brush(plus.doc.layers[0], kW, kD, kSettings);
    const std::vector<MoveWarp> wm = brush::move_brush(minus.doc.layers[0], kRW, kRD, kSettings);
    REQUIRE(nodes_of(wp) == nodes_of(wm));
    for (const MoveWarp& a : wp) {
        const MoveWarp* b = warp_on(wm, a.node);
        REQUIRE(b != nullptr);
        REQUIRE(a.deformers.size() == b->deformers.size());
        for (std::size_t i = 0; i < a.deformers.size(); ++i)
            CHECK(same_grab(a.deformers[i], b->deformers[i]));
    }
    REQUIRE(apply_move(plus.doc, 1, wp) == wp.size());
    REQUIRE(apply_move(minus.doc, 1, wm) == wm.size());
    const std::vector<float> fp = sample(plus.doc, g), fm = sample(minus.doc, g);
    CHECK(differing(fp, fm) == 0);
    CHECK(differing_from_reflection(fp, g) == 0);
    CHECK(differing(fp, undragged) > 100);  // teeth: something moved
}

TEST_CASE("move: an item both images reach takes one grab per image, and frames do not stack") {
    // S straddles the plane, so the ball at x .25 and its reflection at -.25
    // both reach it: two grabs in ONE warp, and a continuing drag replaces
    // both rather than growing the chain 2 -> 4 -> 6 — which is what applying
    // moved_chain once per grab, keyed on the FRONT deformer alone, measured.
    const cfloat3 W = cf3(0.25f, 1.5f, 0);
    const MoveSettings s{0.35f, 0, false};
    Oracle stepped = oracle(true);
    for (float d : {0.05f, 0.10f, 0.15f}) {
        const std::vector<MoveWarp> warps =
            brush::move_brush(stepped.doc.layers[0], W, cf3(d, 0, 0), s);
        REQUIRE(nodes_of(warps) == std::vector<NodeId>{stepped.S});
        REQUIRE(warps[0].deformers.size() == 2);
        REQUIRE(apply_move(stepped.doc, 1, warps) == 1);
        CHECK(stepped.doc.layers[0].sdf->find(stepped.S)->deformers.size() == 2);
    }
    Oracle once = oracle(true);
    REQUIRE(apply_move(once.doc, 1,
                       brush::move_brush(once.doc.layers[0], W, cf3(0.15f, 0, 0), s)) == 1);
    const Grid g(16, 8, 8, 0.05f);
    // Sampled around S, which sits at y 1.5.
    Tape a = compile_document(stepped.doc), b = compile_document(once.doc);
    for (const cfloat3& p : g.pts) {
        const cfloat3 q = p + cf3(0, 1.5f, 0);
        CHECK(a.eval(q).d == b.eval(q).d);
    }
}

TEST_CASE("move: a straddler's grabs are ordered by their values, not by their image") {
    // Off the plane by .08, S is not its own reflection, and two grabs in the
    // other order are a different field. Ordered by which image produced
    // them, the +x drag puts "self" first and the -x drag puts "reflection"
    // first: 325 samples apart on the grid below. Ordered by value the two
    // documents are identical. Reverting the sort (order_by_value) is the
    // verify-to-fail.
    const Grid g(16, 10, 10, 0.05f, cf3(0, 1.5f, 0));  // over S
    const auto make = [] {
        Document doc = symmetric_layer(true);
        add_ball(doc, cf3(0, 0, 0), 0.4f);
        add_ball(doc, cf3(0.08f, 1.5f, 0), 0.2f);
        return doc;
    };
    const MoveSettings s{0.35f, 0, false};
    Document plus = make(), minus = make();
    const std::vector<MoveWarp> wp =
        brush::move_brush(plus.layers[0], cf3(0.25f, 1.5f, 0), cf3(0.15f, 0, 0), s);
    const std::vector<MoveWarp> wm =
        brush::move_brush(minus.layers[0], cf3(-0.25f, 1.5f, 0), cf3(-0.15f, 0, 0), s);
    REQUIRE(wp.size() == 1);
    REQUIRE(wp[0].deformers.size() == 2);
    apply_move(plus, 1, wp);
    apply_move(minus, 1, wm);
    CHECK(differing(sample(plus, g), sample(minus, g)) == 0);
}

TEST_CASE("move: a drag centred on the plane pinches rather than picking a side") {
    // Both images share the centre and pull opposite ways. Two grabs, not
    // one — deduplicating coincident images would keep today's one-sided
    // pull at exactly x = 0 and drop it .03 the moment the centre moves off
    // the plane (measured extents .2805 at x .05, .238 at .01, .2285 at
    // .001, .2275 at 0: continuous), so the rule is the same on the plane as
    // beside it. The order key includes the displacement: keyed on the
    // centre alone the two coincident grabs took the +x drag's order in one
    // document and the -x drag's in the other — 1,365 samples apart on the
    // OFF-CENTRE straddler (an item that is its own reflection cannot tell
    // the two orders apart, which is why both placements are checked).
    const Grid g(16, 10, 10, 0.05f, cf3(0, 1.5f, 0));  // over S
    const auto make = [](float sx) {
        Document doc = symmetric_layer(true);
        add_ball(doc, cf3(0, 0, 0), 0.4f);
        add_ball(doc, cf3(sx, 1.5f, 0), 0.2f);
        return doc;
    };
    const MoveSettings s{0.35f, 0, false};
    for (float sx : {0.0f, 0.08f}) {
        CAPTURE(sx);
        Document plus = make(sx), minus = make(sx);
        const std::vector<MoveWarp> wp =
            brush::move_brush(plus.layers[0], cf3(0, 1.5f, 0), cf3(0.15f, 0, 0), s);
        REQUIRE(wp.size() == 1);
        CHECK(wp[0].deformers.size() == 2);
        apply_move(plus, 1, wp);
        apply_move(minus, 1,
                   brush::move_brush(minus.layers[0], cf3(0, 1.5f, 0), cf3(-0.15f, 0, 0), s));
        CHECK(differing(sample(plus, g), sample(minus, g)) == 0);
    }

    // The pinch: opposite pulls at one centre narrow S along x, symmetrically.
    Document pinched = make(0.0f);
    apply_move(pinched, 1,
               brush::move_brush(pinched.layers[0], cf3(0, 1.5f, 0), cf3(0.15f, 0, 0), s));
    const auto [lo0, hi0] = x_extent(make(0.0f), 1.5f);
    const auto [lo, hi] = x_extent(pinched, 1.5f);
    CHECK(hi0 == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(hi < hi0 - 0.01f);
    CHECK(hi == doctest::Approx(-lo).epsilon(1e-3));
}

TEST_CASE("move: an opted-out item sees the ball, not its reflection") {
    // C opted out of the mirror (Node::mirror = false, the C ABI's -1): the
    // compiler emits no copy of it, so the reflected ball has nothing of C
    // under it and must not warp C's body where no brush is. The same C
    // participating IS reached through its copy. Reverting the participation
    // gate (every item sees every image) selects C with a grab at local
    // x = -0.1 and moves material at (-.85, 0, .5) with the user's ball at +x.
    Oracle opted = oracle(true, /*with_C=*/true);
    CHECK(nodes_of(brush::move_brush(opted.doc.layers[0], kW, kD, kSettings)) ==
          std::vector<NodeId>{opted.A, opted.B});

    Oracle joined = oracle(true, /*with_C=*/true);
    joined.doc.layers[0].sdf->find_mut(joined.C)->mirror = true;
    const std::vector<MoveWarp> warps =
        brush::move_brush(joined.doc.layers[0], kW, kD, kSettings);
    CHECK(nodes_of(warps) == std::vector<NodeId>{joined.A, joined.B, joined.C});
    const MoveWarp* c = warp_on(warps, joined.C);
    REQUIRE(c != nullptr);
    REQUIRE(c->deformers.size() == 1);
    CHECK(std::abs(c->deformers[0].k) < 0.5f);
}

TEST_CASE("move: two mirror axes make two reflections, not four quadrants") {
    // emit_item composes additively — x|y emits two copies of the item, never
    // the x-then-y product — so a drag has 1 + popcount images and the item
    // in the diagonal quadrant is NOT under any of them. Emitting 2^m images
    // would select it and warp it where the compiler put nothing.
    Document doc = symmetric_layer(true);
    doc.layers[0].mirror_axes = kMirrorX | kMirrorY;
    const NodeId self = add_ball(doc, cf3(1.0f, 1.0f, 0), 0.15f, true, 0.0f);
    const NodeId x_image = add_ball(doc, cf3(-1.0f, 1.0f, 0), 0.15f, true, 0.0f);
    const NodeId y_image = add_ball(doc, cf3(1.0f, -1.0f, 0), 0.15f, true, 0.0f);
    add_ball(doc, cf3(-1.0f, -1.0f, 0), 0.15f, true, 0.0f);  // the product quadrant

    const std::vector<brush::DragImage> images =
        brush::drag_images(doc.layers[0], cf3(1.0f, 1.0f, 0), cf3(0.1f, 0, 0));
    CHECK(images.size() == 3);
    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(1.0f, 1.0f, 0), cf3(0.1f, 0, 0), {0.3f, 0, false});
    CHECK(nodes_of(warps) == std::vector<NodeId>{self, x_image, y_image});
}

TEST_CASE("move: radial symmetry rotates the brush the same way") {
    // The same defect through the same bound expansion, and the same fix
    // through the same code path: copy k is the item rotated by +2πk/count,
    // so the image that reaches it is the ball rotated by -2πk/count. B is
    // placed where its first copy sits under the ball at (1, -.3, 0). A
    // rotation is inexact in float, so the rotated-image drag matches the +x
    // drag to a tolerance rather than bit for bit as the mirror does.
    const float quarter = -6.2831853071795864769f / 4.0f;
    const auto make = [&] {
        Document doc = symmetric_layer(false);
        doc.layers[0].radial_count = 4;
        doc.layers[0].radial_axis = 1;
        doc.layers[0].radial_k = 0.05f;
        struct Ids {
            Document doc;
            NodeId base, A, B;
        } r{std::move(doc), kNoNode, kNoNode, kNoNode};
        r.base = add_ball(r.doc, cf3(0, 0, 0), 0.4f);
        r.A = add_ball(r.doc, cf3(1.0f, 0.3f, 0), 0.2f);
        r.B = add_ball(r.doc, kernel::cmul_dir(math::rotation_matrix(1, quarter), cf3(1.0f, -0.3f, 0)),
                       0.2f);
        return r;
    };
    auto plus = make();
    const std::vector<MoveWarp> warps = brush::move_brush(plus.doc.layers[0], kW, kD, kSettings);
    CHECK(nodes_of(warps) == std::vector<NodeId>{plus.A, plus.B});
    const MoveWarp* b = warp_on(warps, plus.B);
    REQUIRE(b != nullptr);
    REQUIRE(b->deformers.size() == 1);
    CHECK(b->deformers[0].k == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(b->deformers[0].a == doctest::Approx(0.3f).epsilon(1e-4));
    CHECK(b->deformers[0].b == doctest::Approx(0.1f).epsilon(1e-4));
    CHECK(b->deformers[0].ext[0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(b->deformers[0].ext[2] == doctest::Approx(0.2f).epsilon(1e-4));

    const auto before = make();
    apply_move(plus.doc, 1, warps);
    // B's copy under the ball moved, as A did.
    const float moved_a = value_at(plus.doc, cf3(1.2f, 0.3f, 0)) - value_at(before.doc, cf3(1.2f, 0.3f, 0));
    const float moved_b = value_at(plus.doc, cf3(1.2f, -0.3f, 0)) - value_at(before.doc, cf3(1.2f, -0.3f, 0));
    CHECK(moved_a < -0.05f);
    CHECK(moved_b == doctest::Approx(moved_a).epsilon(1e-4));

    // The rotated-image drag on a fresh document gives the same field.
    auto rotated = make();
    const math::cfloat4x4 R = math::rotation_matrix(1, quarter);
    apply_move(rotated.doc, 1,
               brush::move_brush(rotated.doc.layers[0], kernel::cmul_dir(R, kW),
                                 kernel::cmul_dir(R, kD), kSettings));
    const Grid g(26, 12, 26, 0.05f);
    const std::vector<float> fp = sample(plus.doc, g), fr = sample(rotated.doc, g);
    for (std::size_t i = 0; i < fp.size(); ++i) CHECK(std::abs(fp[i] - fr[i]) < 1e-6f);
}

TEST_CASE("move: without symmetry the drag has one image and every item one grab") {
    // The rule with one image is byte for byte the rule it replaced.
    Document doc = two_balls();
    const std::vector<brush::DragImage> images =
        brush::drag_images(doc.layers[0], cf3(0.3f, 0.1f, -0.2f), cf3(0, 0.4f, 0));
    REQUIRE(images.size() == 1);
    CHECK(images[0].centre.x == 0.3f);
    CHECK(images[0].centre.y == 0.1f);
    CHECK(images[0].centre.z == -0.2f);
    CHECK(images[0].displacement.y == 0.4f);
    for (const MoveWarp& w : brush::move_brush(doc.layers[0], cf3(0, 0, 0), cf3(0, 0.4f, 0),
                                               MoveSettings{0.8f, 0, false}))
        CHECK(w.deformers.size() == 1);
}

TEST_CASE("move: a non-local item takes one grab per image it sees") {
    // An intersect changes the field everywhere, so its own bound is infinite
    // and every image's ball is on it — one grab per image, exactly as many
    // copies as the compiler emits of it. Opted out, it sees the ball alone.
    for (bool participates : {true, false}) {
        CAPTURE(participates);
        Document doc = symmetric_layer(true);
        add_ball(doc, cf3(0, 0, 0), 0.4f);
        Node cut;
        cut.prim = Prim::sphere(2.0f);
        cut.op = Op::Intersect;
        cut.xform.position = cf3(3.0f, 3.0f, 3.0f);
        cut.mirror = participates;
        const NodeId id = doc.layers[0].sdf->insert(cut);
        const std::vector<MoveWarp> warps =
            brush::move_brush(doc.layers[0], cf3(0.3f, 0, 0), cf3(0.1f, 0, 0), {0.2f, 0, false});
        const MoveWarp* w = warp_on(warps, id);
        REQUIRE(w != nullptr);
        CHECK(w->deformers.size() == (participates ? 2u : 1u));
    }
}

TEST_CASE("move: a feathered volume replace sees the ball alone") {
    // The compiler emits no mirror copy of a feathered replace (its crossfade
    // follows ONE sampled box), so however many images reach it, it takes one
    // grab. The gate is emit_item's, repeated: reverting it to `mirror` alone
    // gives this straddling volume two.
    Document doc = symmetric_layer(true);
    Node vol;
    vol.prim = Prim{PrimType::Volume, {}};
    vol.op = Op::Replace;
    const auto ball = [](cfloat3 p) { return kernel::clength(p) - 0.3f; };
    field::FieldVolume feathered = field::FieldVolume::sample(
        ball, math::Aabb{cf3(-0.5f, -0.5f, -0.5f), cf3(0.5f, 0.5f, 0.5f)}, 0.05f, 0.12f);
    feathered.set_feather(0.08f);
    vol.volume = std::make_shared<field::FieldVolume>(std::move(feathered));
    const NodeId id = doc.layers[0].sdf->insert(vol);
    REQUIRE(scene::item_is_feathered_replace(*doc.layers[0].sdf->find(id)));

    const std::vector<MoveWarp> warps =
        brush::move_brush(doc.layers[0], cf3(0.2f, 0, 0), cf3(0.1f, 0, 0), {0.35f, 0, false});
    REQUIRE(warps.size() == 1);
    CHECK(warps[0].deformers.size() == 1);
}

TEST_CASE("move: a second image that starts reaching mid-drag joins the gesture") {
    // The first frames' pull widens the item's bound (a grab dilates it by
    // its displacement), so an image that missed at frame one can reach at
    // frame two. moved_chain then has one grab to replace and one to add; and
    // when the reach shrinks again (the user drags back) it has two to
    // replace with one, which is why the warp names EVERY image's grab as
    // the gesture's identity and not only the reaching ones — matched on
    // `deformers` alone, frame four below leaves image3 behind with its stale
    // pull. The chain never exceeds the image count and never holds two
    // grabs of one identity.
    Node node;
    node.prim = Prim::sphere(0.2f);
    const Deformer self1 = Deformer::grab(cf3(0.25f, 0, 0), 0.35f, cf3(0.05f, 0, 0));
    const Deformer image1 = Deformer::grab(cf3(-0.25f, 0, 0), 0.35f, cf3(-0.05f, 0, 0));
    const Deformer self2 = Deformer::grab(cf3(0.25f, 0, 0), 0.35f, cf3(0.10f, 0, 0));
    const Deformer image2 = Deformer::grab(cf3(-0.25f, 0, 0), 0.35f, cf3(-0.10f, 0, 0));
    const Deformer self3 = Deformer::grab(cf3(0.25f, 0, 0), 0.35f, cf3(0.15f, 0, 0));
    const Deformer image3 = Deformer::grab(cf3(-0.25f, 0, 0), 0.35f, cf3(-0.15f, 0, 0));
    const auto identities = [](const std::vector<Deformer>& chain) {
        for (std::size_t i = 0; i < chain.size(); ++i)
            for (std::size_t j = i + 1; j < chain.size(); ++j)
                if (chain[i].k == chain[j].k && chain[i].a == chain[j].a &&
                    chain[i].b == chain[j].b && chain[i].c == chain[j].c)
                    return false;
        return true;
    };
    node.deformers = brush::moved_chain(node, MoveWarp{1, {self1}, {self1, image1}});
    CHECK(node.deformers.size() == 1);
    node.deformers = brush::moved_chain(node, MoveWarp{1, {self2, image2}, {self2, image2}});
    CHECK(node.deformers.size() == 2);
    CHECK(identities(node.deformers));
    node.deformers = brush::moved_chain(node, MoveWarp{1, {self3, image3}, {self3, image3}});
    CHECK(node.deformers.size() == 2);
    CHECK(identities(node.deformers));
    CHECK(node.deformers[0].ext[0] == 0.15f);
    node.deformers = brush::moved_chain(node, MoveWarp{1, {self3}, {self3, image3}});
    CHECK(node.deformers.size() == 1);  // both of the gesture's grabs replaced

    // And a resolved warp carries that identity: every image the item sees,
    // split between the grabs that reach it and the rest of the gesture.
    Oracle o = oracle(true);
    const std::vector<MoveWarp> warps = brush::move_brush(o.doc.layers[0], kW, kD, kSettings);
    REQUIRE(!warps.empty());
    for (const MoveWarp& w : warps) {
        CHECK(w.deformers.size() == 1);  // the ball
        CHECK(w.gesture.size() == 1);    // its reflection, seen and not reaching
    }
    // Under no symmetry there is one image and nothing left over: the identity
    // IS the warp, and no second vector is filled per reached item.
    Oracle plain = oracle(false);
    const std::vector<MoveWarp> plain_warps =
        brush::move_brush(plain.doc.layers[0], kW, kD, kSettings);
    REQUIRE(!plain_warps.empty());
    for (const MoveWarp& w : plain_warps) {
        CHECK(w.deformers.size() == 1);
        CHECK(w.gesture.empty());
    }
}

// -- prepare / resolve under symmetry -------------------------------------------
//
// The prepared half carries every IMAGE an item sees — where each lands in the
// item's frame, whether it reaches the item's own bound, and the reflection or
// rotation that makes its displacement — so the per-frame half resolves a
// mirrored or radial drag without the layer: one grab per reaching image,
// ordered by value, and the rest of the gesture's identity. Bit for bit against
// the one-step resolver, so a live drag under a mirror is the commit it
// previews. Reverting the copy's displacement map in `resolve_prepared_move`
// (resolving every image with the world displacement untouched) fails the
// mirror cases at B's grab; dropping `reaches` from the prepared image fails
// them at the deformers/gesture split.

TEST_CASE("move: a prepared drag under symmetry reproduces move_brush exactly") {
    SUBCASE("a mirror: an item under the ball, one under its reflection, an opt-out") {
        Oracle on = oracle(true, /*with_C=*/true);
        for (cfloat3 d : {kD, cf3(0.05f, 0.4f, -0.1f)})
            check_prepare_matches(on.doc.layers[0], kW, d, kSettings);
        // A drag on the plane: both images reach the base with opposite pulls,
        // and the value order decides which grab leads.
        check_prepare_matches(on.doc.layers[0], cf3(0, 0, 0), cf3(0.3f, 0.1f, 0), kSettings);
        // The prepared record says what it saw: B is reached through its copy
        // alone, the base through both images, C (opted out) sees one image.
        const std::vector<brush::PreparedMove> prepared =
            brush::prepare_move(on.doc.layers[0], kW, kSettings);
        for (const brush::PreparedMove& p : prepared) {
            std::size_t reaching = 0;
            for (const brush::PreparedImage& image : p.images) reaching += image.reaches;
            CHECK(reaching >= 1);
            if (p.node == on.B) {
                REQUIRE(p.images.size() == 2);
                CHECK_FALSE(p.images[0].reaches);
                CHECK(p.images[1].reaches);
                CHECK(p.images[1].copy);
            }
            CHECK(p.node != on.C);
        }
    }
    SUBCASE("a mirror under a placed layer transform") {
        Oracle on = oracle(true);
        on.doc.layers[0].xform.position = cf3(0.3f, -0.2f, 0.1f);
        on.doc.layers[0].xform.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), 0.7f);
        on.doc.layers[0].xform.scale = 1.4f;
        const cfloat3 w = on.doc.layers[0].xform.apply(kW);
        check_prepare_matches(on.doc.layers[0], w, cf3(0.2f, 0.1f, 0.05f), kSettings);
        // ...and a rotated item with a per-axis scale under it.
        Node* b = on.doc.layers[0].sdf->find_mut(on.B);
        b->xform.rotation = math::Quat::from_axis_angle(cf3(1, 0, 0), -0.5f);
        b->scale_axes = cf3(1.5f, 1.0f, 0.5f);
        check_prepare_matches(on.doc.layers[0], w, cf3(0.2f, 0.1f, 0.05f), kSettings);
    }
    SUBCASE("two axes") {
        Oracle on = oracle(true);
        on.doc.layers[0].mirror_axes = kMirrorX | kMirrorY;
        check_prepare_matches(on.doc.layers[0], kW, kD, kSettings);
        check_prepare_matches(on.doc.layers[0], cf3(1.0f, -0.3f, 0), cf3(0, -0.2f, 0), kSettings);
    }
    SUBCASE("a radial count") {
        Document doc = symmetric_layer(false);
        doc.layers[0].radial_count = 4;
        doc.layers[0].radial_axis = 1;
        doc.layers[0].radial_k = 0.05f;
        add_ball(doc, cf3(0, 0, 0), 0.4f);
        add_ball(doc, cf3(1.0f, 0.3f, 0), 0.2f);
        add_ball(doc, cf3(0, -0.3f, -1.0f), 0.2f);  // its first copy sits under the ball
        check_prepare_matches(doc.layers[0], kW, kD, kSettings);
        check_prepare_matches(doc.layers[0], cf3(0, 0, 0), cf3(0.3f, 0, 0.1f), kSettings);
    }
}
