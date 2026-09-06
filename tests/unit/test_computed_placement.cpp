// The core of a computed placement (add-convenience-transforms, scene-model
// spec): the three deltas and the ONE write policy the C ABI and pyclay both
// go through.
//
// Two things are asserted here rather than across the C ABI because the C ABI
// cannot express them. A NODE's visibility has no C entry point, so the case
// that says the placement follows the silhouette the artist can SEE has to be
// built on scene::Node directly; and the bit-for-bit claim about what
// translated_layer_command carries through is a claim about the command it
// builds, which is what this reads.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

#include "clay/pick/pick.h"
#include "clay/scene/document.h"
#include "clay/scene/placement.h"

using namespace clay;

namespace {

scene::Node sphere_at(float r, kernel::cfloat3 where) {
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    n.xform.position = where;
    return n;
}

// A layer squashed and turned, which is the placement the whole change exists
// to carry through untouched.
scene::Layer squashed_layer() {
    scene::Layer l;
    l.id = 1;
    l.sdf = std::make_shared<scene::SdfContent>();
    l.xform.position = kernel::cf3(3.0f, 5.0f, -2.0f);
    l.xform.rotation = math::Quat::from_axis_angle(kernel::cf3(0.0f, 0.0f, 1.0f), 0.7f);
    l.xform.scale = 1.25f;
    l.scale_axes = kernel::cf3(1.5f, 0.5f, 2.25f);
    return l;
}

scene::NodeId add_root(scene::Layer& l, scene::Node n) { return l.sdf->insert(std::move(n)); }

}  // namespace

TEST_CASE("the write policy carries everything but the position, bit for bit") {
    scene::Layer l = squashed_layer();
    const kernel::cfloat3 delta = kernel::cf3(0.25f, -7.5f, 3.0f);
    const scene::SetLayerTransformCmd cmd = scene::translated_layer_command(l, delta);

    CHECK(cmd.id == l.id);
    // The rotation quaternion is copied, not rebuilt: the C ABI's readback
    // hands an axis and an angle back through atan2 and its setter rebuilds a
    // quaternion through sin/cos, so composing that public pair -- which is
    // what the change's design.md first proposed -- would NOT be bit-exact.
    CHECK(std::memcmp(&cmd.xform.rotation, &l.xform.rotation, sizeof l.xform.rotation) == 0);
    // And the uniform factor stays in xform.scale rather than being folded into
    // the per-axis triple, which is the other half the public pair would move:
    // its reader answers the PRODUCT of the two.
    CHECK(cmd.xform.scale == l.xform.scale);
    CHECK(cmd.scale_axes.x == l.scale_axes.x);
    CHECK(cmd.scale_axes.y == l.scale_axes.y);
    CHECK(cmd.scale_axes.z == l.scale_axes.z);

    CHECK(cmd.xform.position.x == doctest::Approx(3.25f));
    CHECK(cmd.xform.position.y == doctest::Approx(-2.5f));
    CHECK(cmd.xform.position.z == doctest::Approx(1.0f));
}

TEST_CASE("a delta added to the position is a WORLD translation whatever the squash") {
    // The whole arithmetic of the change: layer_matrix composes
    // xform.matrix() * scale_matrix(scale_axes) with the per-axis scale
    // INNERMOST and the position applied last, so no case analysis over
    // squashed and unsquashed layers is needed and none exists.
    scene::Layer l = squashed_layer();
    add_root(l, sphere_at(1.0f, kernel::cf3(0.0f, 0.0f, 0.0f)));
    add_root(l, sphere_at(0.5f, kernel::cf3(2.0f, 1.0f, 0.0f)));

    const math::Aabb before = pick::layer_bounds(l);
    REQUIRE_FALSE(before.empty());

    const kernel::cfloat3 delta = kernel::cf3(-4.0f, 11.0f, 0.5f);
    const scene::SetLayerTransformCmd cmd = scene::translated_layer_command(l, delta);
    l.xform = cmd.xform;
    l.scale_axes = cmd.scale_axes;
    const math::Aabb after = pick::layer_bounds(l);

    CHECK(after.min.x == doctest::Approx(before.min.x + delta.x));
    CHECK(after.min.y == doctest::Approx(before.min.y + delta.y));
    CHECK(after.min.z == doctest::Approx(before.min.z + delta.z));
    // The box moved and did not change size: the squash is untouched.
    CHECK(after.extent().x == doctest::Approx(before.extent().x));
    CHECK(after.extent().y == doctest::Approx(before.extent().y));
    CHECK(after.extent().z == doctest::Approx(before.extent().z));
}

TEST_CASE("the three rules compute the deltas they say they do") {
    math::Aabb box;
    box.expand(kernel::cf3(1.0f, 4.0f, -3.0f));
    box.expand(kernel::cf3(5.0f, 10.0f, 1.0f));

    const kernel::cfloat3 snap = scene::ground_snap_delta(box, -2.0f);
    CHECK(snap.x == 0.0f);
    CHECK(snap.z == 0.0f);
    CHECK(snap.y == doctest::Approx(-6.0f));

    const kernel::cfloat3 centre = scene::origin_centre_delta(box);
    CHECK(centre.x == doctest::Approx(-3.0f));
    CHECK(centre.y == doctest::Approx(-7.0f));
    CHECK(centre.z == doctest::Approx(1.0f));

    scene::Layer l = squashed_layer();
    const kernel::cfloat3 zero = scene::origin_translation_delta(l);
    // Exactly the negated position, so the placement lands ON the origin and
    // not a rounding away from it.
    CHECK(l.xform.position.x + zero.x == 0.0f);
    CHECK(l.xform.position.y + zero.y == 0.0f);
    CHECK(l.xform.position.z + zero.z == 0.0f);
}

TEST_CASE("a hidden ITEM does not hold the layer up") {
    // pick::layer_bounds skips a root that is not visible, so the placement
    // follows the silhouette the artist can see. That is the INTENDED reading
    // rather than a limitation -- and it means hiding the lowest item changes
    // where the next press lands, which is what this pins.
    scene::Layer l;
    l.id = 1;
    l.sdf = std::make_shared<scene::SdfContent>();
    add_root(l, sphere_at(1.0f, kernel::cf3(0.0f, 0.0f, 0.0f)));
    const scene::NodeId low = add_root(l, sphere_at(1.0f, kernel::cf3(0.0f, -5.0f, 0.0f)));

    const math::Aabb with_low = pick::layer_bounds(l);
    CHECK(with_low.min.y == doctest::Approx(-6.0f));
    {
        const scene::SetLayerTransformCmd cmd =
            scene::translated_layer_command(l, scene::ground_snap_delta(with_low, 0.0f));
        CHECK(cmd.xform.position.y == doctest::Approx(6.0f));
    }

    l.sdf->find_mut(low)->visible = false;
    const math::Aabb without_low = pick::layer_bounds(l);
    CHECK(without_low.min.y == doctest::Approx(-1.0f));
    const scene::SetLayerTransformCmd cmd =
        scene::translated_layer_command(l, scene::ground_snap_delta(without_low, 0.0f));
    CHECK(cmd.xform.position.y == doctest::Approx(1.0f));
}

TEST_CASE("a SUBTRACT item contributes its own box, which the header states") {
    // Not a defect to fix here: pick::layer_bounds expands over every visible
    // root regardless of op. A layer whose lowest visible item subtracts lands
    // THAT box on the plane and the material stops higher up, and a host that
    // does not know will file it as a bug.
    scene::Layer l;
    l.id = 1;
    l.sdf = std::make_shared<scene::SdfContent>();
    add_root(l, sphere_at(1.0f, kernel::cf3(0.0f, 0.0f, 0.0f)));
    scene::Node cut = sphere_at(1.0f, kernel::cf3(0.0f, -3.0f, 0.0f));
    cut.op = scene::Op::Subtract;
    add_root(l, cut);

    CHECK(pick::layer_bounds(l).min.y == doctest::Approx(-4.0f));
}
