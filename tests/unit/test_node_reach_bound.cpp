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
Nested nested(int depth, float k, bool sibling, scene::Op op = scene::Op::Add) {
    Nested n;
    scene::Layer& l = n.doc.add_sdf_layer("l");
    n.layer_id = l.id;
    n.outer = l.sdf->insert(group_node(op, k));
    n.inner = n.outer;
    if (depth >= 2) n.inner = l.sdf->insert(group_node(op, k), n.outer);
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

TEST_CASE("a non-local subtree is still unbounded") {
    // An intersect ANYWHERE above reads the running accumulator and can move
    // the result arbitrarily far. Checked at both levels, because the walk
    // tests op_is_local at every step and a check that only ran on the first
    // would pass the depth-1 case and be wrong for the depth-2 one.
    for (int depth : {1, 2}) {
        CAPTURE(depth);
        Nested n = nested(depth, /*k=*/0.0f, /*sibling=*/false, scene::Op::Intersect);
        const math::Aabb reach = scene::node_reach_bound(n.content(), n.child, n.layer());
        CHECK(reach.is_infinite());
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
