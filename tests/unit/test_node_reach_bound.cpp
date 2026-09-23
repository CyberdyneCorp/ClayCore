#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Where an edit to a node LANDS, which is a different question from where the
// node is.
//
// The answer used to be the node's ROOT ANCESTOR's whole influence bound. That
// was conservative for a real reason — a group's blend spreads a child's
// influence past the child's own box — and much larger than the reason needs:
// a sibling's geometry is not something an edit to the child can reach, so the
// region grew with the size of the GROUP rather than with the size of the
// edit. `node_reach_bound` dilates the child's own bound by each enclosing
// group's blend SUPPORT instead.
//
// Both halves are tested here, and both are needed. That the tight answer is
// small is worth nothing if it is wrong, and that it is correct is worth
// nothing if it is no smaller than what it replaced.

using namespace clay;
using kernel::cf3;

namespace {

// The band-clamped comparison every bound in this engine is stated against:
// outside the bound (dilated by a band), values clamped to +-band are
// unaffected by the edit. Raw far-field values may legitimately shift when a
// smooth-blend operand changes, which is why the guarantee is band-clamped.
constexpr float kBand = 0.15f;

float clamped(const scene::Document& doc, kernel::cfloat3 p) {
    const float d = clay_test::ref_eval_document(doc, p).d;
    return std::clamp(d, -kBand, kBand);
}

// A lattice over a volume comfortably larger than anything these documents
// occupy, so "outside the bound" is actually sampled rather than assumed.
// extent is comfortably past the deepest bound these documents produce: two
// quadratic groups at k = 0.3 support 1.2 each, so a doubly nested child
// reaches 2.6 and the band takes it to 2.75. A lattice that does not leave the
// bound samples nothing, which the REQUIRE below turns into a failure rather
// than a vacuous pass.
std::vector<kernel::cfloat3> lattice(float extent = 5.0f, int side = 25) {
    std::vector<kernel::cfloat3> pts;
    pts.reserve(static_cast<std::size_t>(side) * side * side);
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < side; ++k) {
                auto f = [&](int n) {
                    return static_cast<float>(n) / static_cast<float>(side - 1) * 2 * extent -
                           extent;
                };
                pts.push_back(cf3(f(i), f(j), f(k)));
            }
    return pts;
}

// A smooth-blended group holding `child` and, optionally, a large sibling far
// from it. Returns the child's id.
struct Nested {
    scene::Document doc;
    scene::LayerId layer_id = 0;
    scene::NodeId child = 0;
    scene::NodeId inner = 0;
    scene::NodeId outer = 0;
    scene::NodeId sibling = 0;

    scene::Layer& layer() { return *doc.find_layer(layer_id); }
    scene::SdfContent& content() { return *layer().sdf; }
};

scene::Node group_node(scene::Op op, float k) {
    scene::Node g;
    g.is_group = true;
    g.op = op;
    g.blend.profile = k > 0 ? scene::BlendProfile::Quadratic : scene::BlendProfile::Hard;
    g.blend.k = k;
    return g;
}

// depth 1 or 2 groups; `sibling` adds a far, large child beside the small one.
// `beneath` puts a visible node at the LAYER ROOT, in front of the group.
//
// It is not decoration. After #515 an Add group with nothing accumulated
// beneath it initialises rather than combines -- compile_group emits no empty
// and no combine -- so its blend support cannot move the result and
// node_reach_bound does not dilate by it. A fixture without a left operand
// therefore has no ancestor contribution at all, which is the wrong shape for
// every test here that is ABOUT the ancestor path. The no-left-operand shape is
// covered on its own in test_c_in_group_dirty_reach.cpp.
Nested nested(int depth, float k, bool sibling, scene::Op op = scene::Op::Add,
              bool beneath = true) {
    Nested n;
    scene::Layer& l = n.doc.add_sdf_layer("l");
    n.layer_id = l.id;
    if (beneath)
        l.sdf->insert(clay_test::item(scene::Prim::sphere(0.25f), cf3(0, 2.0f, 0)));
    n.outer = l.sdf->insert(group_node(op, k));
    n.inner = n.outer;
    if (depth >= 2) {
        // The INNER group needs a left operand of its own, inside the outer
        // one, for the same reason the outer group needs one at the root: the
        // first entry in a chain initialises and does not combine, so without
        // this a two-level fixture dilates ONCE and the nesting test is
        // measuring one group while believing it measures two.
        if (beneath)
            l.sdf->insert(clay_test::item(scene::Prim::sphere(0.25f), cf3(0, -2.0f, 0)), n.outer);
        n.inner = l.sdf->insert(group_node(op, k), n.outer);
    }
    n.child = l.sdf->insert(clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 0, 0)), n.inner);
    if (sibling)
        n.sibling =
            l.sdf->insert(clay_test::item(scene::Prim::sphere(0.8f), cf3(2.0f, 0, 0)), n.inner);
    return n;
}

}  // namespace

TEST_CASE("a node inside a group reaches past its own box") {
    // The premise the whole change rests on, asserted rather than assumed: if
    // the child's own bound were the answer, no dilation would be needed and
    // node_reach_bound would be node_influence_bound. It is not.
    Nested n = nested(/*depth=*/1, /*k=*/0.3f, /*sibling=*/true);
    scene::Document& doc = n.doc;

    const math::Aabb own = scene::node_influence_bound(n.content(), n.child, n.layer());
    REQUIRE_FALSE(own.empty());

    // A point outside the child's own bound but inside the group's blend
    // support, on the side facing the sibling — where the smooth weld is.
    const kernel::cfloat3 probe = cf3(own.max.x + 0.1f, 0, 0);
    REQUIRE(probe.x > own.max.x);

    const float before = clamped(doc, probe);
    scene::Node* c = n.content().find_mut(n.child);
    REQUIRE(c != nullptr);
    c->prim = scene::Prim::sphere(0.35f);
    const float after = clamped(doc, probe);

    CHECK(before != after);  // the child's own bound is NOT where its edit lands
}

TEST_CASE("the ancestor-path bound is conservative") {
    // The property every bound here is held to, on one group and on two.
    for (int depth : {1, 2}) {
        CAPTURE(depth);
        Nested n = nested(depth, /*k=*/0.3f, /*sibling=*/true);
        scene::Document& doc = n.doc;

        const math::Aabb reach = scene::node_reach_bound(n.content(), n.child, n.layer());
        REQUIRE_FALSE(reach.empty());
        REQUIRE_FALSE(reach.is_infinite());

        std::vector<kernel::cfloat3> outside;
        std::vector<float> before;
        for (kernel::cfloat3 p : lattice()) {
            // outside the bound DILATED BY THE BAND, which is the form the
            // guarantee takes: a sample keeps its true distance whenever that
            // distance is within the band.
            if (reach.dilated(kBand).contains(p)) continue;
            outside.push_back(p);
            before.push_back(clamped(doc, p));
        }
        REQUIRE(outside.size() > 100);  // the lattice must actually leave the bound

        scene::Node* c = n.content().find_mut(n.child);
        REQUIRE(c != nullptr);
        c->xform.position = cf3(0.15f, 0.05f, -0.05f);
        c->prim = scene::Prim::sphere(0.3f);

        for (std::size_t i = 0; i < outside.size(); ++i) {
            const float now = clamped(doc, outside[i]);
            REQUIRE(now == before[i]);
        }
    }
}

TEST_CASE("a far sibling is not part of the answer") {
    // The half that says the tight bound is actually tighter. Without it the
    // change could "pass" by returning the root's bound under a new name.
    Nested n = nested(/*depth=*/1, /*k=*/0.3f, /*sibling=*/true);

    const math::Aabb reach = scene::node_reach_bound(n.content(), n.child, n.layer());
    const math::Aabb whole = scene::node_influence_bound(n.content(), n.outer, n.layer());
    REQUIRE_FALSE(reach.empty());
    REQUIRE_FALSE(whole.empty());

    CHECK(reach.max.x < whole.max.x);
    // and it does not contain the sibling's geometry
    CHECK_FALSE(reach.contains(cf3(2.0f, 0, 0)));
    CHECK(whole.contains(cf3(2.0f, 0, 0)));
}

TEST_CASE("nested groups each contribute their support") {
    // Two levels dilate twice. Checked as a difference rather than an absolute
    // so it cannot be satisfied by a bound that is merely large.
    Nested one = nested(/*depth=*/1, /*k=*/0.3f, /*sibling=*/false);
    Nested two = nested(/*depth=*/2, /*k=*/0.3f, /*sibling=*/false);

    const math::Aabb a = scene::node_reach_bound(one.content(), one.child, one.layer());
    const math::Aabb b = scene::node_reach_bound(two.content(), two.child, two.layer());
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    CHECK(b.max.x > a.max.x);

    // Exactly one more group's support, taken from the same function the walk
    // uses rather than written out here: a literal would pin the quadratic
    // profile's formula (support is 4k, not k) into a test about NESTING, and
    // would have to be edited whenever a profile's support changed for
    // reasons that have nothing to do with this.
    const float support =
        scene::group_blend_support(*two.content().find(two.outer), two.layer());
    CHECK(b.max.x == doctest::Approx(a.max.x + support).epsilon(0.001));
}

TEST_CASE("an intersecting subtree reaches the LAYER, and a morph reaches further") {
    // An intersect ANYWHERE above reads the running accumulator, so an edit
    // inside it can move the result anywhere the LAYER has material — and no
    // further (#319). A spatial morph above can move it further than that and
    // still answers infinite.
    //
    // Checked at both levels, because the walk tests the op at every step and
    // a check that only ran on the first would pass the depth-1 case and be
    // wrong for the depth-2 one.
    for (int depth : {1, 2}) {
        CAPTURE(depth);
        {
            Nested n = nested(depth, /*k=*/0.0f, /*sibling=*/false, scene::Op::Intersect);
            const math::Aabb reach = scene::node_reach_bound(n.content(), n.child, n.layer());
            CHECK(!reach.is_infinite());
            CHECK(!reach.empty());
        }
        {
            Nested n = nested(depth, /*k=*/0.0f, /*sibling=*/false, scene::Op::TransitionRadial);
            const math::Aabb reach = scene::node_reach_bound(n.content(), n.child, n.layer());
            CHECK(reach.is_infinite());
        }
    }
}

TEST_CASE("a hidden group hides what is inside it") {
    // node_influence_bound reports nothing for a hidden node; an edit inside a
    // hidden group cannot change the field either, and the two must agree.
    Nested n = nested(/*depth=*/1, /*k=*/0.3f, /*sibling=*/false);
    scene::Node* g = n.content().find_mut(n.outer);
    REQUIRE(g != nullptr);
    g->visible = false;
    CHECK(scene::node_reach_bound(n.content(), n.child, n.layer()).empty());
}

TEST_CASE("an item at the layer root reaches exactly its own bound") {
    // No groups above it, so there is nothing to dilate by and the two answers
    // must be identical. This is the case that must NOT change.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::NodeId id = l.sdf->insert(clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));

    const math::Aabb own = scene::node_influence_bound(*l.sdf, id, l);
    const math::Aabb reach = scene::node_reach_bound(*l.sdf, id, l);
    REQUIRE_FALSE(own.empty());
    CHECK(reach.min.x == doctest::Approx(own.min.x));
    CHECK(reach.max.x == doctest::Approx(own.max.x));
}

TEST_CASE("a node command is bounded by the node, not by its root") {
    // The same claim one level up, through the command vocabulary — which is
    // what the invalidation and the undo bound actually call.
    Nested n = nested(/*depth=*/1, /*k=*/0.3f, /*sibling=*/true);

    scene::Node* c = n.content().find_mut(n.child);
    REQUIRE(c != nullptr);
    math::Transform moved = c->xform;
    moved.position = cf3(0.1f, 0, 0);

    const math::Aabb b = scene::command_influence_bound(
        n.doc, scene::Command{scene::SetTransformCmd{n.layer_id, n.child, moved}});
    const math::Aabb whole = scene::node_influence_bound(n.content(), n.outer, n.layer());
    REQUIRE_FALSE(b.empty());
    CHECK(b.max.x < whole.max.x);
    CHECK_FALSE(b.contains(cf3(2.0f, 0, 0)));
}

// -- the combines AFTER a node (#650) -----------------------------------------
//
// A node's own bound says where ITS combine can move the running value. The
// first node of a chain has no combine -- it IS the running value -- and its
// raw distance changes everywhere when it moves. Beyond the band that is
// harmless under a hard union; a SMOOTH combine further down reads the running
// value out to its support and carries the difference back into the band.

namespace {

scene::Node smooth(scene::Node n, float k) {
    n.blend.profile = scene::BlendProfile::Quadratic;
    n.blend.k = k;
    return n;
}

// Two r = 0.3 spheres 0.3 apart, the FIRST hard and the second as given.
// `group` puts both inside a blended group whose own combine does not apply
// (nothing is beneath it), so the only thing dilating the first sphere is
// what follows it.
struct Pair {
    scene::Document doc;
    scene::LayerId layer_id = 0;
    scene::NodeId first = 0;
    scene::NodeId second = 0;

    scene::Layer& layer() { return *doc.find_layer(layer_id); }
    scene::SdfContent& content() { return *layer().sdf; }
};

Pair pair(float second_k, bool group) {
    Pair p;
    scene::Layer& l = p.doc.add_sdf_layer("l");
    p.layer_id = l.id;
    const scene::NodeId parent = group ? l.sdf->insert(group_node(scene::Op::Add, 0.2f))
                                       : scene::kNoNode;
    p.first = l.sdf->insert(clay_test::item(scene::Prim::sphere(0.3f), cf3(-0.5f, 0, 0)), parent);
    scene::Node second = clay_test::item(scene::Prim::sphere(0.3f), cf3(0.4f, 0, 0));
    p.second = l.sdf->insert(second_k > 0.0f ? smooth(second, second_k) : second, parent);
    return p;
}

// Band-clamped samples outside `reach` dilated by the band that moved when
// the first sphere moved 0.1 along x.
std::size_t moved_outside(Pair& p, const math::Aabb& reach) {
    std::vector<kernel::cfloat3> outside;
    std::vector<float> before;
    for (kernel::cfloat3 q : lattice(/*extent=*/2.0f, /*side=*/41)) {
        if (reach.dilated(kBand).contains(q)) continue;
        outside.push_back(q);
        before.push_back(clamped(p.doc, q));
    }
    REQUIRE(outside.size() > 100);
    p.content().find_mut(p.first)->xform.position = cf3(-0.6f, 0, 0);
    std::size_t moved = 0;
    for (std::size_t i = 0; i < outside.size(); ++i)
        if (clamped(p.doc, outside[i]) != before[i]) ++moved;
    return moved;
}

// The node's reach on both sides of the move, unioned: what a host dirties.
math::Aabb swept_reach(Pair& p) {
    math::Aabb reach = scene::node_reach_bound(p.content(), p.first, p.layer());
    scene::Node* n = p.content().find_mut(p.first);
    const math::Transform was = n->xform;
    n->xform.position = cf3(-0.6f, 0, 0);
    reach.expand(scene::node_reach_bound(p.content(), p.first, p.layer()));
    n->xform = was;
    return reach;
}

}  // namespace

TEST_CASE("a smooth sibling after a node carries its edit past the node's own box") {
    // The premise, and the regression: at the layer root and inside a group
    // that does not combine, the first sphere's own bound is not where moving
    // it lands. Measured before #650: in-band samples moved by up to 0.044,
    // 0.25 outside the box dilated by the band.
    for (bool group : {false, true}) {
        CAPTURE(group);
        Pair p = pair(/*second_k=*/0.3f, group);
        math::Aabb own = scene::node_influence_bound(p.content(), p.first, p.layer());
        scene::Node* n = p.content().find_mut(p.first);
        n->xform.position = cf3(-0.6f, 0, 0);
        own.expand(scene::node_influence_bound(p.content(), p.first, p.layer()));
        n->xform.position = cf3(-0.5f, 0, 0);
        CHECK(moved_outside(p, own) > 0);

        Pair q = pair(/*second_k=*/0.3f, group);
        CHECK(moved_outside(q, swept_reach(q)) == 0);
    }
}

TEST_CASE("the drag is the sibling's support, and only a LATER smooth combine adds it") {
    // The half that keeps it tight. A hard sibling after the node drags
    // nothing, and a smooth one BEFORE it never reads the value the node feeds
    // -- the node combines into it, which its own bound already covers.
    Pair hard = pair(/*second_k=*/0.0f, /*group=*/false);
    const math::Aabb own = scene::node_influence_bound(hard.content(), hard.first, hard.layer());
    const math::Aabb hard_reach = scene::node_reach_bound(hard.content(), hard.first, hard.layer());
    CHECK(hard_reach.min.x == own.min.x);
    CHECK(hard_reach.max.x == own.max.x);

    Pair soft = pair(/*second_k=*/0.3f, /*group=*/false);
    const math::Aabb soft_reach = scene::node_reach_bound(soft.content(), soft.first, soft.layer());
    const float support = scene::chain_blend_support(
        scene::Op::Add, soft.content().find(soft.second)->blend, 0.0f);
    CHECK(soft_reach.max.x == doctest::Approx(own.max.x + support).epsilon(0.001));

    // The smooth sphere is the one LATER in the chain, so its own reach is
    // exactly its own bound.
    const math::Aabb last = scene::node_reach_bound(soft.content(), soft.second, soft.layer());
    const math::Aabb last_own =
        scene::node_influence_bound(soft.content(), soft.second, soft.layer());
    CHECK(last.min.x == last_own.min.x);
    CHECK(last.max.x == last_own.max.x);
}

TEST_CASE("a smooth sibling after the node's GROUP drags it too") {
    // The per-level term: a group's children start a chain of their own, and
    // the group's result then feeds the chain OUTSIDE it, where a later smooth
    // sibling reads it.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    const scene::NodeId g = l.sdf->insert(group_node(scene::Op::Add, 0.0f));
    const scene::NodeId child =
        l.sdf->insert(clay_test::item(scene::Prim::sphere(0.3f), cf3(-0.5f, 0, 0)), g);
    const math::Aabb alone = scene::node_reach_bound(*l.sdf, child, l);
    l.sdf->insert(smooth(clay_test::item(scene::Prim::sphere(0.3f), cf3(0.4f, 0, 0)), 0.3f));
    const math::Aabb dragged = scene::node_reach_bound(*l.sdf, child, l);
    CHECK(dragged.max.x > alone.max.x + 1.0f);
}

TEST_CASE("an undo that changes a later sibling's blend re-reads the drag after it") {
    // One step's replay shares a memo of the chain's drag terms across its
    // commands -- a Move step is thousands of them on one chain -- and keeps
    // it only across a command that cannot change those terms. The trap is
    // one that can, between two that read them: undone here, the step first
    // re-bounds the node with its sibling hard, then makes the sibling smooth,
    // then re-bounds the node again. A memo kept across the middle command
    // would answer the last with the hard sibling's zero drag.
    Pair p = pair(/*second_k=*/0.3f, /*group=*/false);
    scene::UndoStack undo;
    const scene::Blend smooth_blend = p.content().find(p.second)->blend;
    scene::Blend hard_blend;
    hard_blend.profile = scene::BlendProfile::Hard;
    // A twist, not a grab: its bound is the node's whole reach, never a ball.
    const scene::Deformer twist = scene::Deformer::twist(0.5f);
    undo.begin_group();
    REQUIRE(undo.perform(p.doc,
                         scene::Command{scene::SetDeformersCmd{p.layer_id, p.first, {twist}}}));
    REQUIRE(undo.perform(p.doc, scene::Command{scene::SetOpBlendCmd{p.layer_id, p.second,
                                                                    scene::Op::Add, hard_blend}}));
    REQUIRE(undo.perform(p.doc, scene::Command{scene::SetDeformersCmd{
                                    p.layer_id, p.first, {twist, twist}}}));
    undo.end_group();

    math::Aabb bound;
    REQUIRE(undo.undo(p.doc, &bound));
    REQUIRE(p.content().find(p.second)->blend.k == smooth_blend.k);
    const math::Aabb own = scene::node_influence_bound(p.content(), p.first, p.layer());
    const float support = scene::chain_blend_support(scene::Op::Add, smooth_blend, 0.0f);
    CHECK(bound.min.x <= own.min.x - support + 1e-4f);
}

TEST_CASE("the chain-drag memo answers exactly what the walk does") {
    // A memo is only an optimisation if nothing can tell it fired. Every node
    // of a nested document with mixed hard and smooth siblings, queried in an
    // order that makes the memo fill some chains from the middle and extend
    // them later, against a fresh walk per query.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    std::vector<scene::NodeId> ids;
    const scene::NodeId outer = l.sdf->insert(group_node(scene::Op::Add, 0.1f));
    const scene::NodeId inner = l.sdf->insert(group_node(scene::Op::Add, 0.05f), outer);
    ids.push_back(outer);
    ids.push_back(inner);
    for (int i = 0; i < 12; ++i) {
        scene::Node n = clay_test::item(scene::Prim::sphere(0.2f),
                                        cf3(0.3f * static_cast<float>(i) - 1.5f, 0, 0));
        if (i % 3 == 1) n = smooth(n, 0.02f * static_cast<float>(i));
        const scene::NodeId parent = i < 4 ? scene::kNoNode : i < 8 ? outer : inner;
        ids.push_back(l.sdf->insert(n, parent));
    }
    std::reverse(ids.begin() + 6, ids.end());
    scene::LayerExtent memo;
    for (scene::NodeId id : ids) {
        CAPTURE(id);
        const math::Aabb walked = scene::node_influence_bound_in_document(doc, *l.sdf, id);
        const math::Aabb memoized = scene::node_influence_bound_in_document(doc, *l.sdf, id, &memo);
        CHECK(walked.min.x == memoized.min.x);
        CHECK(walked.max.x == memoized.max.x);
        CHECK(walked.min.y == memoized.min.y);
        CHECK(walked.max.z == memoized.max.z);
    }
}
