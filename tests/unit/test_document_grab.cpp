// Document grab — the Move brush (brush-engine spec, add-document-grab).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/brush/grab.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;

namespace {

brush::GrabSettings drag(kernel::cfloat3 centre, float radius, kernel::cfloat3 disp) {
    brush::GrabSettings s;
    s.centre = centre;
    s.radius = radius;
    s.displacement = disp;
    return s;
}

// Apply a resolved plan through the ordinary command vocabulary, which is how a
// host would: if this stops working the resolver is useless however correct it is.
std::size_t apply_plan(scene::Document& doc, const std::vector<brush::GrabTarget>& plan) {
    std::size_t applied = 0;
    for (const brush::GrabTarget& t : plan) {
        scene::SetDeformersCmd cmd{t.layer, t.node, t.deformers};
        if (scene::apply(doc, scene::Command{cmd})) ++applied;
    }
    return applied;
}

// Where the surface sits along +Y above (x, z). Scanned DOWNWARD, from above
// the form: scanning up from below starts outside the shape and reports the
// first sample every time.
float top_at(const scene::Tape& t, float x, float z = 0.0f) {
    for (float y = 2.2f; y > -1.6f; y -= 0.002f)
        if (t.eval(cf3(x, y, z)).d <= 0.0f) return y;
    return -99.0f;  // nothing in this column
}

scene::Node ball(float r, kernel::cfloat3 at) {
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    n.xform.position = at;
    return n;
}

// Two overlapping balls blended into one form — the case the whole row exists
// for, and the one where grabbing a single item is visibly wrong.
scene::Document two_balls() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    for (float x : {-0.35f, 0.35f}) {
        scene::Node n = ball(0.5f, cf3(x, 0, 0));
        n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.25f};
        l.sdf->insert(std::move(n));
    }
    return doc;
}

}  // namespace

TEST_CASE("document grab: a multi-item form moves as one surface") {
    scene::Document before = two_balls();
    scene::Document after = two_balls();

    std::vector<brush::GrabTarget> plan =
        brush::grab_document(after, drag(cf3(0, 0, 0), 1.2f, cf3(0, 0.4f, 0)));
    REQUIRE(plan.size() == 2);  // both items are within reach
    REQUIRE(apply_plan(after, plan) == 2);

    scene::Tape a = scene::compile_document(before);
    scene::Tape b = scene::compile_document(after);

    // Every part of the surface inside the drag rises, and by a comparable
    // amount — which is exactly what grabbing ONE item fails to do.
    std::vector<float> lift;
    for (float x = -0.6f; x <= 0.61f; x += 0.1f) lift.push_back(top_at(b, x) - top_at(a, x));
    for (std::size_t i = 0; i < lift.size(); ++i) {
        CAPTURE(i);
        CHECK(lift[i] > 0.05f);
    }
    const float lo = *std::min_element(lift.begin(), lift.end());
    const float hi = *std::max_element(lift.begin(), lift.end());
    INFO("lift ranged " << lo << " to " << hi);
    CHECK(hi - lo < 0.05f);  // one surface, not two items pulled separately

    SUBCASE("and it is symmetric, because the drag is") {
        for (float x = 0.05f; x <= 0.6f; x += 0.1f) {
            CAPTURE(x);
            CHECK(top_at(b, x) == doctest::Approx(top_at(b, -x)).epsilon(0.02));
        }
    }
}

TEST_CASE("document grab: grabbing one item is what this replaces") {
    // The measurement that motivated the row, kept as a test so the difference
    // cannot quietly disappear. Both do the SAME world drag, centred on the
    // origin between the two balls; they differ only in how many items carry it.
    scene::Document plain = two_balls();
    scene::Tape a = scene::compile_document(plain);

    scene::Document one_item = two_balls();
    {
        scene::Layer& l = one_item.layers.front();
        scene::Node* left = l.sdf->find_mut(l.sdf->roots.front());
        REQUIRE(left != nullptr);
        // local (0.35,0,0) on an item at world (-0.35,0,0) IS the world origin
        left->deformers.push_back(scene::Deformer::grab(cf3(0.35f, 0, 0), 1.2f, cf3(0, 0.4f, 0)));
    }
    scene::Document whole = two_balls();
    REQUIRE(apply_plan(whole, brush::grab_document(
                                  whole, drag(cf3(0, 0, 0), 1.2f, cf3(0, 0.4f, 0)))) == 2);

    auto tilt = [&a](const scene::Document& d) {
        scene::Tape t = scene::compile_document(d);
        return (top_at(t, -0.35f) - top_at(a, -0.35f)) - (top_at(t, 0.35f) - top_at(a, 0.35f));
    };
    const float lopsided = tilt(one_item);
    const float even = tilt(whole);
    INFO("left minus right: " << lopsided << " grabbing one item, " << even
                              << " grabbing the surface");
    CHECK(lopsided > 0.04f);          // one item pulls its own share and no more
    CHECK(std::abs(even) < 0.01f);    // the resolver moves the surface as one
    CHECK(std::abs(even) < lopsided);
}

TEST_CASE("document grab: the mapping does not care how the transform is split") {
    // The strongest statement of correctness available without reimplementing
    // the warp in the test: the same world surface, expressed two ways, must
    // come back the same after the same world drag.
    const math::Transform placement{cf3(0.6f, -0.2f, 0.15f),
                                    math::Quat::from_axis_angle(cf3(0.3f, 1, 0.2f), 0.7f),
                                    1.4f};

    scene::Document on_node;
    {
        scene::Layer& l = on_node.add_sdf_layer("l");
        scene::Node n = ball(0.5f, cf3(0, 0, 0));
        n.xform = placement;
        l.sdf->insert(std::move(n));
    }
    scene::Document on_layer;
    {
        scene::Layer& l = on_layer.add_sdf_layer("l");
        l.xform = placement;
        l.sdf->insert(ball(0.5f, cf3(0, 0, 0)));
    }

    brush::GrabSettings s = drag(cf3(0.6f, 0.3f, 0.15f), 0.9f, cf3(0.2f, 0.35f, -0.1f));
    s.ease = 3;
    REQUIRE(apply_plan(on_node, brush::grab_document(on_node, s)) == 1);
    REQUIRE(apply_plan(on_layer, brush::grab_document(on_layer, s)) == 1);

    scene::Tape a = scene::compile_document(on_node);
    scene::Tape b = scene::compile_document(on_layer);
    for (float x = -1.0f; x <= 1.6f; x += 0.083f)
        for (float y = -1.0f; y <= 1.2f; y += 0.091f) {
            kernel::cfloat3 p = cf3(x, y, 0.13f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(a.eval(p).d == doctest::Approx(b.eval(p).d).epsilon(1e-4));
        }
}

TEST_CASE("document grab: on a single untransformed item it is the plain deformer") {
    scene::Document resolved;
    resolved.add_sdf_layer("l").sdf->insert(ball(0.7f, cf3(0, 0, 0)));
    scene::Document by_hand;
    {
        scene::Node n = ball(0.7f, cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::grab(cf3(0, 0.5f, 0), 0.8f, cf3(0.3f, 0.2f, 0)));
        by_hand.add_sdf_layer("l").sdf->insert(std::move(n));
    }
    REQUIRE(apply_plan(resolved, brush::grab_document(
                                     resolved, drag(cf3(0, 0.5f, 0), 0.8f, cf3(0.3f, 0.2f, 0)))) ==
            1);

    scene::Tape a = scene::compile_document(resolved);
    scene::Tape b = scene::compile_document(by_hand);
    for (float x = -1.2f; x <= 1.2f; x += 0.07f)
        for (float y = -1.2f; y <= 1.4f; y += 0.079f) {
            kernel::cfloat3 p = cf3(x, y, 0.05f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(a.eval(p).d == doctest::Approx(b.eval(p).d));
        }
}

TEST_CASE("document grab: a drag reaches only what it can touch") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(ball(0.4f, cf3(0, 0, 0)));
    const scene::NodeId far_id = l.sdf->insert(ball(0.4f, cf3(4.0f, 0, 0)));

    std::vector<brush::GrabTarget> plan =
        brush::grab_document(doc, drag(cf3(0, 0, 0), 0.9f, cf3(0, 0.3f, 0)));
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().node != far_id);

    SUBCASE("and the distant item's field is untouched") {
        scene::Tape before = scene::compile_document(doc);
        REQUIRE(apply_plan(doc, plan) == 1);
        scene::Tape after = scene::compile_document(doc);
        for (float x = 3.2f; x <= 4.8f; x += 0.05f)
            for (float y = -0.6f; y <= 0.6f; y += 0.07f) {
                kernel::cfloat3 p = cf3(x, y, 0.03f);
                CAPTURE(x);
                CAPTURE(y);
                CHECK(after.eval(p).d == doctest::Approx(before.eval(p).d));
            }
    }

    SUBCASE("so a local gesture does not cost the whole document its step scale") {
        scene::Document one_only = doc;
        REQUIRE(apply_plan(one_only, plan) == 1);
        // The far item keeps its exactness, so only the grabbed item's cost is
        // folded in — not two items' worth for a drag that touched one.
        scene::Document both = doc;
        for (scene::NodeId id : both.layers.front().sdf->roots)
            both.layers.front().sdf->find_mut(id)->deformers.push_back(
                scene::Deformer::grab(cf3(0, 0, 0), 0.9f, cf3(0, 0.3f, 0)));
        const float local = kernel::csafe_step_scale(scene::compile_document(one_only).info);
        const float all = kernel::csafe_step_scale(scene::compile_document(both).info);
        INFO("step scale " << local << " culled vs " << all << " unculled");
        CHECK(local >= all);
    }
}

TEST_CASE("document grab: a drag coalesces instead of accumulating") {
    scene::Document doc;
    doc.add_sdf_layer("l").sdf->insert(ball(0.6f, cf3(0, 0, 0)));

    // One drag, growing: centre and radius fixed, displacement growing.
    for (float d : {0.05f, 0.12f, 0.25f, 0.4f}) {
        std::vector<brush::GrabTarget> plan =
            brush::grab_document(doc, drag(cf3(0, 0.4f, 0), 0.9f, cf3(0, d, 0)));
        REQUIRE(plan.size() == 1);
        CAPTURE(d);
        CHECK(plan.front().deformers.size() == 1);  // replaced, never appended
        REQUIRE(apply_plan(doc, plan) == 1);
    }
    const scene::Node* n = doc.layers.front().sdf->find(doc.layers.front().sdf->roots.front());
    REQUIRE(n != nullptr);
    CHECK(n->deformers.size() == 1);
    CHECK(n->deformers.back().ext[1] == doctest::Approx(0.4f));  // the latest drag

    SUBCASE("but a different drag is a different deformer") {
        std::vector<brush::GrabTarget> plan =
            brush::grab_document(doc, drag(cf3(0.3f, -0.2f, 0), 0.9f, cf3(0.1f, 0, 0)));
        REQUIRE(plan.size() == 1);
        CHECK(plan.front().deformers.size() == 2);  // the first drag is kept
    }
}

TEST_CASE("document grab: resolving reads the document and changes nothing") {
    scene::Document doc = two_balls();
    std::vector<std::uint8_t> before = scene::serialize_document(doc);
    std::vector<brush::GrabTarget> plan =
        brush::grab_document(doc, drag(cf3(0, 0, 0), 1.5f, cf3(0.2f, 0.2f, 0)));
    REQUIRE_FALSE(plan.empty());
    CHECK(scene::serialize_document(doc) == before);
}

TEST_CASE("document grab: hidden and protected layers are not in the plan") {
    scene::Document doc;
    doc.add_sdf_layer("visible").sdf->insert(ball(0.5f, cf3(0, 0, 0)));
    doc.add_sdf_layer("hidden").sdf->insert(ball(0.5f, cf3(0, 0, 0)));
    doc.add_sdf_layer("locked").sdf->insert(ball(0.5f, cf3(0, 0, 0)));
    doc.layers[1].visible = false;
    doc.layers[2].locked = true;

    std::vector<brush::GrabTarget> plan =
        brush::grab_document(doc, drag(cf3(0, 0, 0), 1.0f, cf3(0, 0.2f, 0)));
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().layer == doc.layers.front().id);
    // Everything the plan contains is an edit the document will accept.
    CHECK(apply_plan(doc, plan) == plan.size());
}

TEST_CASE("document grab: a drag with no reach is empty, not an error") {
    scene::Document doc = two_balls();
    CHECK(brush::grab_document(doc, drag(cf3(9, 9, 9), 0.5f, cf3(0, 0.2f, 0))).empty());
    CHECK(brush::grab_document(doc, drag(cf3(0, 0, 0), 0.0f, cf3(0, 0.2f, 0))).empty());
}

TEST_CASE("document grab: a mirrored item is grabbed symmetrically") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.mirror_axes = 1u;  // X
    scene::Node n = ball(0.45f, cf3(0.5f, 0, 0));
    n.mirror = true;
    l.sdf->insert(std::move(n));

    scene::Tape before = scene::compile_document(doc);
    REQUIRE(apply_plan(doc, brush::grab_document(
                               doc, drag(cf3(0.5f, 0.3f, 0), 0.7f, cf3(0, 0.3f, 0)))) == 1);
    scene::Tape after = scene::compile_document(doc);

    // The drag moved the near copy...
    CHECK(top_at(after, 0.5f) > top_at(before, 0.5f) + 0.05f);
    // ...and the mirror copy went with it, because the chain is local and every
    // copy evaluates the same chain. That is what symmetric sculpting wants.
    for (float x = 0.2f; x <= 0.8f; x += 0.1f) {
        CAPTURE(x);
        CHECK(top_at(after, -x) == doctest::Approx(top_at(after, x)).epsilon(0.02));
    }
}

TEST_CASE("set deformers: the command inverts and round-trips") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    const scene::NodeId id = l.sdf->insert(ball(0.6f, cf3(0, 0, 0)));
    std::vector<std::uint8_t> pristine = scene::serialize_document(doc);

    scene::SetDeformersCmd cmd{
        l.id, id, {scene::Deformer::grab(cf3(0, 0.4f, 0), 0.8f, cf3(0.2f, 0.1f, 0), 2, true)}};
    std::optional<scene::Command> inverse = scene::apply(doc, scene::Command{cmd});
    REQUIRE(inverse.has_value());
    CHECK(scene::serialize_document(doc) != pristine);

    SUBCASE("its inverse restores the document exactly") {
        REQUIRE(scene::apply(doc, *inverse).has_value());
        CHECK(scene::serialize_document(doc) == pristine);
    }

    SUBCASE("the command itself survives serialization") {
        std::vector<std::uint8_t> bytes = scene::serialize(scene::Command{cmd});
        std::optional<scene::Command> back = scene::deserialize(bytes.data(), bytes.size());
        REQUIRE(back.has_value());
        const auto* got = std::get_if<scene::SetDeformersCmd>(&*back);
        REQUIRE(got != nullptr);
        REQUIRE(got->deformers.size() == 1);
        CHECK(got->node == id);
        CHECK(got->deformers[0].type == kernel::cdeform_grab);
        CHECK(got->deformers[0].c == doctest::Approx(0.8f));
        CHECK(got->deformers[0].ext[3] == doctest::Approx(1.0f));  // front_only
    }

    SUBCASE("and the deformed document round-trips") {
        std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
        auto reloaded = scene::deserialize_document(bytes.data(), bytes.size());
        REQUIRE(reloaded.has_value());
        CHECK(scene::serialize_document(*reloaded) == bytes);
    }
}

TEST_CASE("set deformers: it refuses what every edit refuses") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    const scene::NodeId id = l.sdf->insert(ball(0.6f, cf3(0, 0, 0)));
    const scene::LayerId lid = l.id;

    CHECK_FALSE(scene::apply(doc, scene::Command{scene::SetDeformersCmd{lid, 9999, {}}})
                    .has_value());
    CHECK_FALSE(scene::apply(doc, scene::Command{scene::SetDeformersCmd{4242, id, {}}})
                    .has_value());

    doc.layers.front().locked = true;
    CHECK_FALSE(scene::apply(doc, scene::Command{scene::SetDeformersCmd{lid, id, {}}})
                    .has_value());
}

TEST_CASE("set deformers: bumping the scene minor did not break older documents") {
    // This row took kSceneMinor from 4 to 5 by adding a command tag. The NODE
    // encoding is untouched, so every earlier minor must still read — and a
    // deformed document is the case that would break first if it ever did.
    //
    // Read at the minor it was WRITTEN at, which is what the container does:
    // the scene payload carries no version of its own, and .clayspace passes
    // the one from its own header down.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = ball(0.7f, cf3(0, 0, 0));
    n.deformers.push_back(scene::Deformer::grab(cf3(0, 0.3f, 0), 0.8f, cf3(0, 0.2f, 0)));
    l.sdf->insert(std::move(n));
    const float reference = scene::compile_document(doc).eval(cf3(0, 0.5f, 0)).d;

    for (std::uint16_t minor : {std::uint16_t(2), std::uint16_t(3), std::uint16_t(4),
                                scene::kSceneMinor}) {
        CAPTURE(minor);
        std::vector<std::uint8_t> bytes = scene::serialize_document(doc, minor);
        auto back = scene::deserialize_document(bytes.data(), bytes.size(), minor);
        REQUIRE(back.has_value());
        CHECK(scene::compile_document(*back).eval(cf3(0, 0.5f, 0)).d ==
              doctest::Approx(reference));
    }
}
