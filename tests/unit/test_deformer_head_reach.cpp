#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <optional>
#include <vector>

#include "clay/kernel/ease.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Where changing the HEAD of a deformer chain can change the field (#639).
//
// `deformer_head_reach_in_document` claims more than a band-clamped bound: a
// finite-support link is the identity outside its ball, so the RAW field is
// bit-identical outside the box it reports. That is the claim this file tests,
// against the reference evaluator, on the raw value -- a stronger check than
// the brick oracle in test_c_undo_bound_grab_support.cpp, which sees only what
// the cache stores. And it tests the refusals, which are what keep the answer
// from ever being a guess.

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

struct Fixture {
    scene::Document doc;
    scene::LayerId layer_id = 0;
    scene::NodeId node = 0;

    scene::Layer& layer() { return *doc.find_layer(layer_id); }
    scene::SdfContent& content() { return *layer().sdf; }
    std::vector<scene::Deformer>& chain() { return content().find_mut(node)->deformers; }
};

// A sphere of radius 0.6 at `at` in a fresh layer, carrying `chain`.
Fixture sphere_with(std::vector<scene::Deformer> chain, cfloat3 at = cf3(0, 0, 0)) {
    Fixture f;
    scene::Layer& l = f.doc.add_sdf_layer("l");
    f.layer_id = l.id;
    scene::Node n = clay_test::item(scene::Prim::sphere(0.6f), at);
    n.deformers = std::move(chain);
    f.node = l.sdf->insert(n);
    return f;
}

std::optional<math::Aabb> reach(Fixture& f, const std::vector<scene::Deformer>& before,
                                const std::vector<scene::Deformer>& after) {
    return scene::deformer_head_reach_in_document(f.doc, f.content(), f.node, before, after);
}

std::vector<cfloat3> lattice(float extent, int side) {
    std::vector<cfloat3> pts;
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < side; ++k) {
                auto f = [&](int n) {
                    return -extent + 2.0f * extent * static_cast<float>(n) /
                                         static_cast<float>(side - 1);
                };
                pts.push_back(cf3(f(i), f(j), f(k)));
            }
    return pts;
}

struct Sampled {
    std::size_t outside = 0;  // lattice points outside the box
    std::size_t moved = 0;    // of those, how many changed at all
    std::size_t changed = 0;  // points anywhere whose value changed
};

// Swap the node's chain from `before` to `after` and compare the RAW field,
// bit for bit, at every lattice point outside `box`.
Sampled sample_outside(Fixture& f, const math::Aabb& box, const std::vector<scene::Deformer>& before,
                       const std::vector<scene::Deformer>& after) {
    const std::vector<cfloat3> pts = lattice(2.5f, 41);
    f.chain() = before;
    std::vector<float> a;
    for (const cfloat3& p : pts) a.push_back(clay_test::ref_eval_document(f.doc, p).d);
    f.chain() = after;
    Sampled s;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const float b = clay_test::ref_eval_document(f.doc, pts[i]).d;
        const bool same = std::bit_cast<std::uint32_t>(a[i]) == std::bit_cast<std::uint32_t>(b);
        if (!same) ++s.changed;
        if (box.contains(pts[i])) continue;
        ++s.outside;
        if (!same) ++s.moved;
    }
    return s;
}

// A finite, non-empty box, or the test has nothing to compare against.
math::Aabb finite_reach(Fixture& f, const std::vector<scene::Deformer>& before,
                        const std::vector<scene::Deformer>& after) {
    const std::optional<math::Aabb> box = reach(f, before, after);
    REQUIRE(box.has_value());
    REQUIRE((!box->empty() && !box->is_infinite()));
    return *box;
}

// The claim, for one fixture: the edit is visible somewhere (so a pass is not
// vacuous), the box leaves something outside it to compare, and the raw field
// is untouched everywhere out there.
void check_exact_outside(Fixture& f, const std::vector<scene::Deformer>& before,
                         const std::vector<scene::Deformer>& after) {
    const Sampled s = sample_outside(f, finite_reach(f, before, after), before, after);
    CAPTURE(s.changed);
    CAPTURE(s.outside);
    REQUIRE((s.changed > 0 && s.outside > 0));
    CHECK(s.moved == 0);
}

scene::Deformer grab_at(cfloat3 c, float r, cfloat3 d) { return scene::Deformer::grab(c, r, d); }

std::vector<scene::Deformer> with_head(scene::Deformer head, std::vector<scene::Deformer> tail) {
    tail.insert(tail.begin(), head);
    return tail;
}

const std::vector<scene::Deformer> kTail = {
    grab_at(cf3(0.6f, 0, 0), 0.25f, cf3(0.1f, 0, 0)),
    grab_at(cf3(-0.6f, 0, 0), 0.25f, cf3(-0.1f, 0, 0)),
};

}  // namespace

TEST_CASE("head reach: a grab at the head changes the raw field only inside its box") {
    Fixture f = sphere_with(kTail);
    const auto after = with_head(grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0)), kTail);
    check_exact_outside(f, kTail, after);   // redo: the grab appears
    check_exact_outside(f, after, kTail);   // undo: the grab goes
}

TEST_CASE("head reach: magnify and blob qualify beside grab, and each is exact outside") {
    const std::vector<scene::Deformer> heads = {
        scene::Deformer::magnify(cf3(0, 0.6f, 0), 0.3f, 0.5f),
        scene::Deformer::blob(cf3(0, 0.6f, 0), 0.3f, 0.08f),
    };
    for (const scene::Deformer& head : heads) {
        CAPTURE(static_cast<int>(head.type));
        Fixture f = sphere_with(kTail);
        check_exact_outside(f, kTail, with_head(head, kTail));
    }
}

TEST_CASE("head reach: a grab reports its displaced end as well as its centre") {
    Fixture f = sphere_with({});
    const auto after = std::vector{grab_at(cf3(0, 0.6f, 0), 0.2f, cf3(0.5f, 0, 0))};
    const std::optional<math::Aabb> box = reach(f, {}, after);
    REQUIRE(box.has_value());
    CHECK(box->contains(cf3(0.5f + 0.19f, 0.6f, 0)));  // the far end's ball
    CHECK(box->contains(cf3(-0.19f, 0.6f, 0)));         // the centre's ball
}

TEST_CASE("head reach: a radial pose is refused, because it is not the identity outside") {
    // Finite support by its weight, and still not the identity past it:
    // cpose_point has no zero-weight early-out, so outside the ball it returns
    // `centre + (p - centre)` rotated by zero, which is NOT p in float. The
    // field moves by an ulp everywhere the item is. Found by the raw check
    // below: 1,917 of 68,796 lattice points outside the ball moved.
    const auto pose = scene::Deformer::pose(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0, 1), 0.6f);
    Fixture f = sphere_with(kTail);
    CHECK_FALSE(reach(f, kTail, with_head(pose, kTail)).has_value());
    const Sampled s = sample_outside(f, math::Aabb{cf3(-0.3f, 0.3f, -0.3f), cf3(0.3f, 0.9f, 0.3f)},
                                     kTail, with_head(pose, kTail));
    CHECK(s.moved > 0);  // the reason, pinned: if this ever reads 0, pose can join the list
}

TEST_CASE("head reach: the same chain on both sides changes nothing") {
    Fixture f = sphere_with(kTail);
    const std::optional<math::Aabb> box = reach(f, kTail, kTail);
    REQUIRE(box.has_value());
    CHECK(box->empty());
}

TEST_CASE("head reach: a link removed from the MIDDLE counts the links ahead of it too") {
    // The common tail stops at the removed link, so the links in front of it
    // are part of both heads: a point one of them moves can be moved INTO the
    // removed link's ball, and the union of all their balls covers that.
    const auto before = std::vector{grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0)),
                                    grab_at(cf3(0, -0.6f, 0), 0.3f, cf3(0, -0.2f, 0)),
                                    kTail[0]};
    const auto after = std::vector{before[0], before[2]};
    Fixture f = sphere_with(before);
    const std::optional<math::Aabb> box = reach(f, before, after);
    REQUIRE(box.has_value());
    CHECK(box->contains(cf3(0, 0.6f, 0)));
    CHECK(box->contains(cf3(0, -0.6f, 0)));
    check_exact_outside(f, before, after);
}

TEST_CASE("head reach: a whole-item deformer anywhere in the head is refused") {
    // A twist ahead of the grab turns the point before the grab tests its
    // ball, so the region is the twist's preimage of the ball, not the ball.
    Fixture f = sphere_with({});
    const auto after = std::vector{scene::Deformer::twist(0.5f),
                                   grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0))};
    CHECK_FALSE(reach(f, {}, after).has_value());
    // ...and a grab appended BEHIND an existing twist is the same case.
    const auto twisted = std::vector{scene::Deformer::twist(0.5f)};
    const auto appended = std::vector{twisted[0], grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0))};
    CHECK_FALSE(reach(f, twisted, appended).has_value());
    // A whole-item deformer in the common TAIL costs nothing: it sees the
    // point the head hands it, which is unchanged outside the ball.
    const auto head_on_twist =
        std::vector{grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0)), twisted[0]};
    CHECK(reach(f, twisted, head_on_twist).has_value());
}

TEST_CASE("head reach: an easing that is not exactly zero at the rim is refused") {
    // ease_out_sine is 1 - (1 - cos(pi/2)) at the rim, which is not zero in
    // float: a grab under it translates the whole item by a hair, so its
    // "support" is everywhere.
    REQUIRE(kernel::cease(kernel::ease_out_sine, 0.0f) != 0.0f);
    Fixture f = sphere_with({});
    auto g = grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0));
    g.ease = kernel::ease_out_sine;
    CHECK_FALSE(reach(f, {}, std::vector{g}).has_value());
    // ...and the refusal is the ease's, not the grab's.
    g.ease = kernel::ease_smoothstep;
    CHECK(reach(f, {}, std::vector{g}).has_value());
}

TEST_CASE("head reach: every easing it accepts is exactly zero at the rim") {
    // The list in bounds.cpp refuses the families whose rim runs through a
    // transcendental; everything it accepts must at least be zero on the host.
    for (int e = 0; e < kernel::ease_count; ++e) {
        CAPTURE(e);
        Fixture f = sphere_with({});
        auto g = grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0));
        g.ease = static_cast<std::uint8_t>(e);
        if (reach(f, {}, std::vector{g}).has_value())
            CHECK(kernel::cease(e, 0.0f) == 0.0f);
    }
}

TEST_CASE("head reach: the ball is placed where the item is -- transform, mirror, repeat") {
    SUBCASE("a rotated, scaled item under a moved layer") {
        Fixture f = sphere_with(kTail, cf3(0.3f, -0.2f, 0.1f));
        scene::Node& n = *f.content().find_mut(f.node);
        n.xform.rotation = math::Quat::from_axis_angle(cf3(0, 0, 1), 0.7f);
        n.xform.scale = 1.3f;
        f.layer().xform.position = cf3(-0.2f, 0.1f, 0);
        check_exact_outside(f, kTail, with_head(grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0)),
                                                kTail));
    }
    SUBCASE("a mirrored layer, where the ball has a reflection") {
        Fixture f = sphere_with(kTail, cf3(0.9f, 0, 0));
        f.layer().mirror_axes = scene::kMirrorX;
        f.layer().mirror_k = 0.05f;
        check_exact_outside(f, kTail, with_head(grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0)),
                                                kTail));
    }
    SUBCASE("a finite repeat grid, where the ball recurs in every cell") {
        Fixture f = sphere_with({});
        scene::Node& n = *f.content().find_mut(f.node);
        n.prim = scene::Prim::sphere(0.3f);
        n.repeat.type = kernel::crepeat_grid_finite;
        n.repeat.spacing = cf3(1.0f, 1.0f, 1.0f);
        n.repeat.counts = cf3(1, 0, 0);
        check_exact_outside(f, {}, std::vector{grab_at(cf3(0, 0.3f, 0), 0.2f, cf3(0, 0.15f, 0))});
    }
}

TEST_CASE("head reach: an infinite repeat grid is refused") {
    Fixture f = sphere_with({});
    scene::Node& n = *f.content().find_mut(f.node);
    n.repeat.type = kernel::crepeat_grid_infinite;
    n.repeat.spacing = cf3(2.0f, 2.0f, 2.0f);
    CHECK_FALSE(reach(f, {}, std::vector{grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0))})
                    .has_value());
}

TEST_CASE("head reach: dilated once per enclosing group and by every fold above") {
    Fixture f;
    scene::Layer& base = f.doc.add_sdf_layer("base");
    base.sdf->insert(clay_test::item(scene::Prim::sphere(0.5f), cf3(0, -3.0f, 0)));
    scene::Layer& l = f.doc.add_sdf_layer("l");
    f.layer_id = l.id;
    l.composition.op = scene::Op::Add;
    l.composition.blend.profile = scene::BlendProfile::Quadratic;
    l.composition.blend.k = 0.05f;
    // A node beneath the group so its combine really combines (#515).
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 2.0f, 0)));
    scene::Node g;
    g.is_group = true;
    g.op = scene::Op::Add;
    g.blend.profile = scene::BlendProfile::Quadratic;
    g.blend.k = 0.1f;
    const scene::NodeId group = l.sdf->insert(g);
    f.node = l.sdf->insert(clay_test::item(scene::Prim::sphere(0.6f), cf3(0, 0, 0)), group);

    const auto head = std::vector{grab_at(cf3(0, 0.6f, 0), 0.2f, cf3(0, 0.1f, 0))};
    const std::optional<math::Aabb> box = reach(f, {}, head);
    REQUIRE(box.has_value());
    // The ball (radius 0.2 about y = 0.6, and about y = 0.7 at the displaced
    // end), then the group's support (4 * 0.1) and the fold's (4 * 0.05):
    // 0.2 + 0.4 + 0.2 on every side of the two balls' hull.
    const float grow = 0.2f + 0.4f + 0.2f;
    CHECK(box->max.y == doctest::Approx(0.7f + grow).epsilon(0.01));
    CHECK(box->min.y == doctest::Approx(0.6f - grow).epsilon(0.01));
    CHECK(box->min.x == doctest::Approx(-grow).epsilon(0.01));
    check_exact_outside(f, {}, head);
}

TEST_CASE("head reach: a morph group above is refused, a hidden one too") {
    scene::Node g;
    g.is_group = true;
    g.op = scene::Op::TransitionLinear;
    Fixture m;
    scene::Layer& l = m.doc.add_sdf_layer("l");
    m.layer_id = l.id;
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 2.0f, 0)));
    const scene::NodeId group = l.sdf->insert(g);
    m.node = l.sdf->insert(clay_test::item(scene::Prim::sphere(0.6f), cf3(0, 0, 0)), group);
    const auto head = std::vector{grab_at(cf3(0, 0.6f, 0), 0.2f, cf3(0, 0.1f, 0))};
    CHECK_FALSE(reach(m, {}, head).has_value());
    l.sdf->find_mut(group)->op = scene::Op::Add;
    CHECK(reach(m, {}, head).has_value());
    l.sdf->find_mut(group)->visible = false;
    CHECK_FALSE(reach(m, {}, head).has_value());
}

TEST_CASE("head reach: an instanced layer places the ball once per sharer") {
    Fixture f = sphere_with(kTail);
    scene::Layer* copy = f.doc.instance_layer(f.layer_id, "copy");
    REQUIRE(copy != nullptr);
    copy->xform.position = cf3(3.0f, 0, 0);
    const auto after = with_head(grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0)), kTail);
    const std::optional<math::Aabb> box = reach(f, kTail, after);
    REQUIRE(box.has_value());
    CHECK(box->contains(cf3(0, 0.6f, 0)));
    CHECK(box->contains(cf3(3.0f, 0.6f, 0)));
}

TEST_CASE("head reach: the command side reads the node's chain before the apply") {
    Fixture f = sphere_with(kTail);
    const auto after = with_head(grab_at(cf3(0, 0.6f, 0), 0.3f, cf3(0, 0.2f, 0)), kTail);
    const scene::Command cmd{scene::SetDeformersCmd{f.layer_id, f.node, after}};
    const std::optional<math::Aabb> box = scene::command_head_delta_bound(f.doc, cmd);
    REQUIRE(box.has_value());
    CHECK(box->contains(cf3(0, 0.6f, 0)));
    // Another command kind is never narrowed here.
    const scene::Command other{scene::SetColorCmd{f.layer_id, f.node, cf3(1, 0, 0)}};
    CHECK_FALSE(scene::command_head_delta_bound(f.doc, other).has_value());
}

namespace {

// A 2x2x2 cage about the sphere with one corner dragged, so it is not the
// identity and a change to it is visible.
scene::Deformer dragged_lattice(float corner_dx) {
    scene::Deformer d = scene::Deformer::lattice(cf3(-0.7f, -0.7f, -0.7f), cf3(0.7f, 0.7f, 0.7f),
                                                 2, 2, 2);
    d.cage[0] = cf3(corner_dx, 0, 0);
    return d;
}

std::vector<scene::StrokePoint> guide_through(float bow) {
    std::vector<scene::StrokePoint> g(3);
    g[0].pos = cf3(0, -0.7f, 0);
    g[1].pos = cf3(bow, 0, 0);
    g[2].pos = cf3(0, 0.7f, 0);
    return g;
}

}  // namespace

TEST_CASE("head reach: a lattice or a bend curve in the common tail is stripped") {
    // The tail sees the same point on both sides, whatever it does with it.
    const auto head = grab_at(cf3(0, 0.6f, 0), 0.25f, cf3(0, 0.15f, 0));
    const std::vector<std::vector<scene::Deformer>> tails = {
        {dragged_lattice(0.1f)},
        {scene::Deformer::bend_curve(guide_through(0.1f), 0.0f, 1.0f)},
    };
    for (const auto& tail : tails) {
        CAPTURE(static_cast<int>(tail[0].type));
        Fixture f = sphere_with(tail);
        check_exact_outside(f, tail, with_head(head, tail));
    }
}

TEST_CASE("head reach: a payload link that differs only in its payload is refused") {
    // Same type, same record: only the cage or the guide moved. Comparing the
    // record alone would call them one link, strip them as common tail, and
    // report the grab's ball for a change that reaches the whole item.
    const auto head = grab_at(cf3(0, 0.6f, 0), 0.25f, cf3(0, 0.15f, 0));
    SUBCASE("the cage") {
        const std::vector<scene::Deformer> before = {head, dragged_lattice(0.1f)};
        const std::vector<scene::Deformer> after = {head, dragged_lattice(0.2f)};
        Fixture f = sphere_with(before);
        CHECK_FALSE(reach(f, before, after).has_value());
    }
    SUBCASE("the cage's placement") {
        scene::Deformer moved = dragged_lattice(0.1f);
        moved.cage_xform.position = cf3(0.05f, 0, 0);
        const std::vector<scene::Deformer> before = {head, dragged_lattice(0.1f)};
        const std::vector<scene::Deformer> after = {head, moved};
        Fixture f = sphere_with(before);
        CHECK_FALSE(reach(f, before, after).has_value());
    }
    SUBCASE("the guide") {
        const std::vector<scene::Deformer> before = {
            head, scene::Deformer::bend_curve(guide_through(0.1f), 0.0f, 1.0f)};
        const std::vector<scene::Deformer> after = {
            head, scene::Deformer::bend_curve(guide_through(0.2f), 0.0f, 1.0f)};
        Fixture f = sphere_with(before);
        CHECK_FALSE(reach(f, before, after).has_value());
    }
}
