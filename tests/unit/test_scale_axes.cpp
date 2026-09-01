// A per-axis scale on a placed item (#320, scene-model / c-abi / file-io).
//
// Every transform in the interface took ONE scale factor, so a slot (a squashed
// capsule), an oval bolt hole (a squashed cylinder) and a stretched chamfer (a
// box longer on one axis) had no route: the primitives that carry their own
// extents could say it at creation and never afterwards, and the ones that do
// not could not say it at all.
//
// The operator was already in the kernel and already classified —
// cscale_nu_point / cscale_nu_dist, cfi_scale_nonuniform — and unused by
// anything but one kernel test. These cases pin what exposing it means.

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/scene/types.h"

using namespace clay;

namespace {

scene::Document one_sphere(kernel::cfloat3 axes, float uniform = 1.0f) {
    scene::Document doc;
    scene::Layer layer;
    layer.id = doc.reserve_layer_id();
    layer.sdf = std::make_shared<scene::SdfContent>();
    scene::Node n;
    n.id = layer.sdf->reserve_id();
    n.prim = scene::Prim::sphere(1.0f);
    n.xform.scale = uniform;
    n.scale_axes = axes;
    layer.sdf->insert(std::move(n), scene::kNoNode, -1);
    doc.layers.push_back(std::move(layer));
    return doc;
}

float eval_tape(const scene::Tape& tape, kernel::cfloat3 p) {
    const float xyz[3] = {p.x, p.y, p.z};
    float out = 0.0f;
    eval::PointQuery q;
    q.points_xyz = xyz;
    q.count = 1;
    eval::PointResults r;
    r.distances = &out;
    eval::eval_points_reference(tape, q, r);
    return out;
}

float eval_at(const scene::Document& doc, kernel::cfloat3 p) {
    return eval_tape(scene::compile_document(doc), p);
}

}  // namespace

TEST_CASE("a per-axis scale stretches the surface and nothing else") {
    // A unit sphere scaled 2x on X is an ellipsoid: the surface crosses x at 2
    // and y at 1. That is the whole feature, and the sphere primitive has no
    // parameter that could say it.
    scene::Document doc = one_sphere(kernel::cf3(2.0f, 1.0f, 1.0f));

    CHECK(eval_at(doc, kernel::cf3(2.0f, 0, 0)) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_at(doc, kernel::cf3(0, 1.0f, 0)) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_at(doc, kernel::cf3(0, 0, 1.0f)) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_at(doc, kernel::cf3(0, 0, 0)) < 0.0f);     // inside
    CHECK(eval_at(doc, kernel::cf3(3.0f, 0, 0)) > 0.0f);  // outside

    // The bound follows, or the cull would drop a shape that is on screen.
    const math::Aabb b = scene::item_geometry_bound(*doc.layers[0].sdf->find(1), doc.layers[0]);
    CHECK(b.max.x == doctest::Approx(2.0f));
    CHECK(b.max.y == doctest::Approx(1.0f));
}

TEST_CASE("the value is a conservative bound, never an overestimate") {
    // The property that makes this safe rather than merely plausible: with the
    // distance multiplied back by the SMALLEST component, the reported value
    // never exceeds the true distance to the surface, so a marcher stepping by
    // it cannot cross. Sampled around the ellipsoid rather than argued.
    scene::Document doc = one_sphere(kernel::cf3(4.0f, 1.0f, 1.0f));
    scene::Tape tape = scene::compile_document(doc);

    // The true distance is not analytic for an ellipsoid, so it is bounded from
    // below by marching: from p, stepping by the reported value must never
    // land inside.
    for (int i = 0; i < 24; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 24.0f;
        const kernel::cfloat3 p = kernel::cf3(6.0f * std::cos(a), 6.0f * std::sin(a), 0.0f);
        const float d = eval_tape(tape, p);
        REQUIRE(d > 0.0f);
        // One full step along the inward ray must not overshoot the surface.
        CHECK(eval_tape(tape, p - kernel::cnormalize(p) * d) >= -1e-4f);
    }
}

TEST_CASE("a per-axis scale costs exactness and not step size") {
    // The cost, stated exactly. cscale_nu_dist can only SHORTEN a distance, so
    // the Lipschitz bound does not move and no marcher slows down; what goes is
    // the guarantee that the value IS the distance.
    scene::Tape squashed = scene::compile_document(one_sphere(kernel::cf3(2.0f, 1.0f, 1.0f)));
    scene::Tape uniform = scene::compile_document(one_sphere(kernel::cf3(1.0f, 1.0f, 1.0f), 2.0f));

    CHECK(uniform.info.is_exact == true);
    CHECK(squashed.info.is_exact == false);
    CHECK(squashed.info.lipschitz == doctest::Approx(uniform.info.lipschitz));
    CHECK(kernel::csafe_step_scale(squashed.info) ==
          doctest::Approx(kernel::csafe_step_scale(uniform.info)));
}

TEST_CASE("a UNIFORM per-axis scale is exactly the uniform scale") {
    // (s, s, s) is a similarity, so it must stay exact and must evaluate
    // identically to the uniform factor — otherwise every host that sets three
    // equal numbers silently loses exactness for nothing.
    scene::Tape three = scene::compile_document(one_sphere(kernel::cf3(2.0f, 2.0f, 2.0f)));
    scene::Tape one = scene::compile_document(one_sphere(kernel::cf3(1.0f, 1.0f, 1.0f), 2.0f));
    CHECK(three.info.is_exact == true);

    for (int i = 0; i < 8; ++i) {
        const kernel::cfloat3 p = kernel::cf3(0.4f * static_cast<float>(i) - 1.0f, 0.3f, -0.2f);
        CHECK(eval_tape(three, p) == doctest::Approx(eval_tape(one, p)).epsilon(1e-5));
    }
}

TEST_CASE("the default per-axis scale changes no tape at all") {
    // The regression that matters most: every document that existed before this
    // field must compile to the bytes it always did. Compared as the whole
    // parameter block, not as a spot check.
    scene::Document plain = one_sphere(kernel::cf3(1.0f, 1.0f, 1.0f), 1.25f);
    scene::Tape a = scene::compile_document(plain);
    scene::Document same = one_sphere(kernel::cf3(1.0f, 1.0f, 1.0f), 1.25f);
    scene::Tape b = scene::compile_document(same);
    CHECK(a.params == b.params);
    CHECK(a.info.is_exact == true);
}

TEST_CASE("a per-axis scale survives a save and reload") {
    scene::Document doc = one_sphere(kernel::cf3(2.0f, 0.5f, 3.0f));
    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    const scene::Node* n = back->layers.at(0).sdf->find(1);
    REQUIRE(n != nullptr);
    CHECK(n->scale_axes.x == doctest::Approx(2.0f));
    CHECK(n->scale_axes.y == doctest::Approx(0.5f));
    CHECK(n->scale_axes.z == doctest::Approx(3.0f));
    // And it round trips to identical bytes, so the field is not merely
    // readable but stable.
    CHECK(scene::serialize_document(*back) == bytes);
}

TEST_CASE("writing at an older minor degrades a squash to its uniform scale") {
    // The one-directional loss, made explicit: a build that predates the field
    // must read the bytes it always did, and the item it gets back is the round
    // one rather than a missing one. Recoverable and obvious, which is the
    // direction this format takes everywhere else.
    scene::Document doc = one_sphere(kernel::cf3(2.0f, 0.5f, 3.0f), 1.5f);
    const std::vector<std::uint8_t> old_bytes = scene::serialize_document(doc, 13);
    // The bytes a build that predates the field would have written, exactly:
    // an unsquashed document at minor 13 serializes identically.
    CHECK(old_bytes == scene::serialize_document(one_sphere(kernel::cf3(1, 1, 1), 1.5f), 13));

    std::optional<scene::Document> back =
        scene::deserialize_document(old_bytes.data(), old_bytes.size(), 13);
    REQUIRE(back.has_value());
    const scene::Node* n = back->layers.at(0).sdf->find(1);
    REQUIRE(n != nullptr);
    CHECK(n->scale_axes.x == doctest::Approx(1.0f));
    CHECK(n->scale_axes.y == doctest::Approx(1.0f));
    CHECK(n->scale_axes.z == doctest::Approx(1.0f));
    CHECK(n->xform.scale == doctest::Approx(1.5f));  // the uniform half survives
}

TEST_CASE("setting a transform is one undo step that restores both scales") {
    scene::Document doc = one_sphere(kernel::cf3(2.0f, 1.0f, 1.0f));
    const scene::LayerId lid = doc.layers[0].id;

    scene::SetTransformCmd cmd{lid, 1, math::Transform{}, kernel::cf3(1.0f, 4.0f, 1.0f)};
    std::optional<scene::Command> inverse = scene::apply(doc, scene::Command{cmd});
    REQUIRE(inverse.has_value());
    CHECK(doc.layers[0].sdf->find(1)->scale_axes.y == doctest::Approx(4.0f));

    // The inverse captured what was there, per-axis scale included — without
    // that, one undo of a squash leaves the item squashed the other way.
    REQUIRE(scene::apply(doc, *inverse).has_value());
    const scene::Node* n = doc.layers[0].sdf->find(1);
    CHECK(n->scale_axes.x == doctest::Approx(2.0f));
    CHECK(n->scale_axes.y == doctest::Approx(1.0f));
}

// -- the LAYER's per-axis scale (#373) ---------------------------------------

TEST_CASE("a layer written at an older minor degrades to its uniform scale") {
    // The same one-directional loss the item arm states above, one level out. A
    // build that predates the field must read the bytes it always did and get
    // back the unsquashed subtool rather than a broken one — recoverable and
    // obvious, which is the direction this format takes everywhere.
    scene::Document doc = one_sphere(kernel::cf3(1, 1, 1), 1.0f);
    doc.layers[0].xform.scale = 1.5f;
    doc.layers[0].scale_axes = kernel::cf3(3.0f, 0.5f, 2.0f);

    const std::vector<std::uint8_t> old_bytes = scene::serialize_document(doc, 15);
    // The bytes a build that predates the field would have written, exactly: an
    // unsquashed layer at minor 15 serializes identically.
    scene::Document plain = one_sphere(kernel::cf3(1, 1, 1), 1.0f);
    plain.layers[0].xform.scale = 1.5f;
    CHECK(old_bytes == scene::serialize_document(plain, 15));

    std::optional<scene::Document> back =
        scene::deserialize_document(old_bytes.data(), old_bytes.size(), 15);
    REQUIRE(back.has_value());
    CHECK(back->layers.at(0).scale_axes.x == doctest::Approx(1.0f));
    CHECK(back->layers.at(0).scale_axes.y == doctest::Approx(1.0f));
    CHECK(back->layers.at(0).scale_axes.z == doctest::Approx(1.0f));
    CHECK(back->layers.at(0).xform.scale == doctest::Approx(1.5f));  // the uniform half survives
}

TEST_CASE("a layer's per-axis scale round-trips at the current minor") {
    scene::Document doc = one_sphere(kernel::cf3(1, 1, 1), 1.0f);
    doc.layers[0].scale_axes = kernel::cf3(3.0f, 0.5f, 2.0f);
    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc, scene::kSceneMinor);
    std::optional<scene::Document> back =
        scene::deserialize_document(bytes.data(), bytes.size(), scene::kSceneMinor);
    REQUIRE(back.has_value());
    CHECK(back->layers.at(0).scale_axes.x == doctest::Approx(3.0f));
    CHECK(back->layers.at(0).scale_axes.y == doctest::Approx(0.5f));
    CHECK(back->layers.at(0).scale_axes.z == doctest::Approx(2.0f));

    // And the field it describes comes back with it, which is what a
    // round-tripped placement is actually for.
    const scene::Tape a = scene::compile_document(doc);
    const scene::Tape b = scene::compile_document(*back);
    CHECK(a.params == b.params);
    CHECK(a.instrs.size() == b.instrs.size());
}

TEST_CASE("setting a layer transform is one undo step that restores both scales") {
    scene::Document doc = one_sphere(kernel::cf3(1, 1, 1), 1.0f);
    doc.layers[0].scale_axes = kernel::cf3(2.0f, 1.0f, 1.0f);
    const scene::LayerId lid = doc.layers[0].id;

    scene::SetLayerTransformCmd cmd{lid, math::Transform{}, kernel::cf3(1.0f, 4.0f, 1.0f)};
    std::optional<scene::Command> inverse = scene::apply(doc, scene::Command{cmd});
    REQUIRE(inverse.has_value());
    CHECK(doc.layers[0].scale_axes.y == doctest::Approx(4.0f));

    // The inverse captured what was there, per-axis scale included — without
    // that, one undo of a subtool squash leaves it squashed the other way.
    REQUIRE(scene::apply(doc, *inverse).has_value());
    CHECK(doc.layers[0].scale_axes.x == doctest::Approx(2.0f));
    CHECK(doc.layers[0].scale_axes.y == doctest::Approx(1.0f));
}

TEST_CASE("a layer scale and an item scale multiply, in that order") {
    // The composition the whole change rests on:
    //     layer.xform * diag(L) * node.xform * diag(N)
    // A 2x layer on a 3x item is 6x, and the two are not interchangeable when
    // there is a rotation between them — which is why the order is pinned here
    // rather than left to the arithmetic.
    scene::Document doc = one_sphere(kernel::cf3(3.0f, 1.0f, 1.0f), 1.0f);
    doc.layers[0].scale_axes = kernel::cf3(2.0f, 1.0f, 1.0f);
    const scene::Tape tape = scene::compile_document(doc);

    // The surface of a unit sphere scaled 3x on X by the item and 2x again by
    // the layer sits at x = 6.
    CHECK(tape.eval(kernel::cf3(6.0f, 0, 0)).d == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(tape.eval(kernel::cf3(0, 1.0f, 0)).d == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(tape.eval(kernel::cf3(7.0f, 0, 0)).d > 0.0f);

    // Conservative everywhere along the stretched axis: the reported distance
    // never exceeds the true one.
    for (float x = 0.2f; x < 8.0f; x += 0.21f) {
        const float got = tape.eval(kernel::cf3(x, 0, 0)).d;
        CHECK(std::fabs(got) <= std::fabs(x - 6.0f) + 1e-4f);
    }
}
