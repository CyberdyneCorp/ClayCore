// A mask that gates any operation (sdf-kernels + scene-model deltas of
// add-masking-that-gates-any-op).
//
// Masks gated AUTHORING before this: a voxel edit consumed one per cell as it
// wrote, and an SDF edit consumed one when a stroke became items. Neither
// touched an item already in the edit list, so a mask over an ear did nothing
// about the next BOOLEAN — which is the operation an artist most wants
// protection from, and what masking means in every other sculptor.
//
// The claims worth defending: a fully protected region is EXACTLY what it was
// before the item combined, an absent gate is EXACTLY the ungated result, the
// declared step scale is honest enough to march by, and an ungated document
// pays nothing for the feature existing.

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "clay/brush/mask_extrude.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/voxel/mask.h"

using namespace clay;
using namespace clay::kernel;
using field::FieldVolume;
using voxel::MaskField;

namespace {

constexpr float kBody = 1.0f;

// A mask over the +X half of the body, as the signed distance a gate reads.
std::shared_ptr<const FieldVolume> half_gate(float from_x = 0.1f) {
    MaskField m(0.05f);
    m.fill(math::Aabb{cf3(from_x, -0.8f, -0.8f), cf3(1.6f, 0.8f, 0.8f)}, 1.0f);
    std::optional<FieldVolume> v = brush::mask_to_field(m, 0.5f, 0.25f, 0.3f);
    REQUIRE(v.has_value());
    return std::make_shared<const FieldVolume>(std::move(*v));
}

// A ball with a channel subtracted through it, the subtract optionally gated.
scene::Document body_with_channel(std::shared_ptr<const FieldVolume> gate, float width = 0.1f) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node body;
    body.prim = scene::Prim::sphere(kBody);
    l.sdf->insert(body);
    scene::Node cut;
    cut.prim = scene::Prim::box(cf3(2.0f, 0.25f, 0.25f));
    cut.op = scene::Op::Subtract;
    if (gate) {
        cut.gate = std::move(gate);
        cut.gate_width = width;
    }
    l.sdf->insert(cut);
    return doc;
}

scene::Document bare_body() {
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(kBody);
    doc.add_sdf_layer("l").sdf->insert(body);
    return doc;
}

}  // namespace

TEST_CASE("a mask protects a surface from a boolean") {
    // The motivating case, and the one nothing in this engine could do before:
    // cut a channel through a body and the masked half is untouched.
    const scene::Tape ungated = scene::compile_document(body_with_channel(nullptr));
    const scene::Tape gated = scene::compile_document(body_with_channel(half_gate()));
    const scene::Tape solid = scene::compile_document(bare_body());

    // In the channel on the MASKED side the subtract did not happen: the field
    // is the solid body's, exactly.
    const cfloat3 protected_point = cf3(0.7f, 0, 0);
    CHECK(ungated.eval(protected_point).d > 0.0f);  // carved away without the gate
    CHECK(gated.eval(protected_point).d == solid.eval(protected_point).d);

    // On the OPEN side it acted normally.
    const cfloat3 open_point = cf3(-0.7f, 0, 0);
    CHECK(ungated.eval(open_point).d > 0.0f);
    CHECK(gated.eval(open_point).d == ungated.eval(open_point).d);

    // And away from the channel entirely, both agree with each other.
    for (const cfloat3 p : {cf3(0.7f, 0.6f, 0), cf3(0, 2.0f, 0), cf3(-0.7f, 0.6f, 0)})
        CHECK(gated.eval(p).d == ungated.eval(p).d);
}

TEST_CASE("a gate is exact at both ends") {
    // Not approximately: a gate that only nearly restored the accumulator would
    // leave a seam along the mask's border, and one that only nearly released
    // would leave a ghost of the item over the whole open region.
    const scene::Tape gated = scene::compile_document(body_with_channel(half_gate()));
    const scene::Tape ungated = scene::compile_document(body_with_channel(nullptr));
    const scene::Tape solid = scene::compile_document(bare_body());

    // Exactly, ACROSS the whole protected region rather than at three lucky
    // points. This is the case that catches the last ULP: cmix(x, y, t) is
    // x + (y - x) * t, so t == 1 returns y only up to rounding — which would
    // put a one-float seam along the border of every protected region. The
    // kernel branches at the fully-protected end for that reason.
    for (int i = 0; i <= 60; ++i) {
        const float x = 0.35f + static_cast<float>(i) * (1.05f / 60.0f);
        const cfloat3 p = cf3(x, 0, 0);
        CAPTURE(x);
        REQUIRE(gated.eval(p).d == solid.eval(p).d);
    }

    // Well inside the protected region: exactly the accumulator.
    for (float x : {0.6f, 0.8f, 1.0f}) {
        const cfloat3 p = cf3(x, 0, 0);
        CAPTURE(x);
        REQUIRE(gated.eval(p).d == solid.eval(p).d);
    }
    // Well outside it: exactly the ungated result.
    for (float x : {-0.5f, -0.8f, -1.1f}) {
        const cfloat3 p = cf3(x, 0, 0);
        CAPTURE(x);
        REQUIRE(gated.eval(p).d == ungated.eval(p).d);
    }
}

TEST_CASE("an ungated document pays nothing for gates existing") {
    // The gate is one comparison per combine and a -1 in a slot. What must not
    // change is the field, the bound, or the bytes.
    const scene::Document doc = body_with_channel(nullptr);
    const scene::Tape t = scene::compile_document(doc);
    CHECK(t.safe_step_scale() == doctest::Approx(1.0f));

    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    const std::optional<scene::Document> back =
        scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);

    const scene::Tape again = scene::compile_document(*back);
    for (int i = 0; i < 256; ++i) {
        const cfloat3 p =
            cf3(static_cast<float>(i % 7 - 3) * 0.35f, static_cast<float>((i / 7) % 7 - 3) * 0.35f,
                static_cast<float>((i / 49) % 7 - 3) * 0.35f);
        REQUIRE(t.eval(p).d == again.eval(p).d);
    }
}

TEST_CASE("a gate declares its cost, and a wider one costs less") {
    // Mixing two fields by a spatially varying weight is not a distance, and
    // saying otherwise lets a marcher walk through the surface. The weight is a
    // smoothstep of a DISTANCE across the gate's width, so the cost is known in
    // advance and falls as the width grows — which is the trade to expose
    // rather than hide.
    const scene::Tape ungated = scene::compile_document(body_with_channel(nullptr));
    CHECK(ungated.safe_step_scale() == doctest::Approx(1.0f));

    float previous = 0.0f;
    for (float width : {0.05f, 0.1f, 0.3f, 0.8f}) {
        const scene::Tape t = scene::compile_document(body_with_channel(half_gate(), width));
        CAPTURE(width);
        CAPTURE(t.safe_step_scale());
        CHECK(t.safe_step_scale() < ungated.safe_step_scale());  // a gate is never free
        CHECK(t.safe_step_scale() > previous);                   // ...and wider is cheaper
        previous = t.safe_step_scale();
    }
}

TEST_CASE("marching by the declared step scale does not overshoot a gated surface") {
    // The bound is only worth having if marching by it is safe. This is the
    // case that fails if the mix is charged too little — and a renderer would
    // show it as holes punched through the masked boundary.
    const scene::Tape t = scene::compile_document(body_with_channel(half_gate(), 0.15f));
    const float scale = t.safe_step_scale();
    REQUIRE(scale > 0.0f);

    // Rays crossing the mask's border, where the weight actually varies.
    for (int i = 0; i < 32; ++i) {
        const float y = (static_cast<float>(i % 8) - 3.5f) * 0.12f;
        const float z = (static_cast<float>(i / 8) - 1.5f) * 0.12f;
        const cfloat3 origin = cf3(-3.0f, y, z);
        const cfloat3 dir = cf3(1, 0, 0);
        float travelled = 0.0f;
        for (int step = 0; step < 4000 && travelled < 6.0f; ++step) {
            const float d = t.eval(origin + dir * travelled).d;
            if (d <= 1e-4f) break;  // arrived
            const float advance = scale * d;
            const float next = t.eval(origin + dir * (travelled + advance)).d;
            CAPTURE(i);
            CAPTURE(step);
            CAPTURE(travelled);
            CAPTURE(d);
            CAPTURE(next);
            REQUIRE(next >= -1e-3f);  // float slack only
            travelled += advance;
        }
    }
}

TEST_CASE("a gate survives the document, and an older reader loses the gate not the item") {
    const scene::Document doc = body_with_channel(half_gate(), 0.2f);
    const scene::Tape original = scene::compile_document(doc);

    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    const std::optional<scene::Document> back =
        scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);  // canonical

    const scene::Tape reloaded = scene::compile_document(*back);
    for (int i = 0; i < 256; ++i) {
        const cfloat3 p =
            cf3(static_cast<float>(i % 7 - 3) * 0.3f, static_cast<float>((i / 7) % 7 - 3) * 0.3f,
                static_cast<float>((i / 49) % 7 - 3) * 0.3f);
        REQUIRE(original.eval(p).d == reloaded.eval(p).d);
    }
    CHECK(reloaded.safe_step_scale() == doctest::Approx(original.safe_step_scale()));

    // Written at an older minor, the gate is dropped and the ITEM remains — a
    // downgrade rather than a different document. The channel is cut all the
    // way through, which is what that build could always have shown.
    const std::vector<std::uint8_t> older = scene::serialize_document(doc, 10);
    const std::optional<scene::Document> old_read =
        scene::deserialize_document(older.data(), older.size(), 10);
    REQUIRE(old_read.has_value());
    const scene::Tape degraded = scene::compile_document(*old_read);
    const scene::Tape ungated = scene::compile_document(body_with_channel(nullptr));
    const cfloat3 was_protected = cf3(0.7f, 0, 0);
    CHECK(degraded.eval(was_protected).d == ungated.eval(was_protected).d);
    CHECK(degraded.safe_step_scale() == doctest::Approx(ungated.safe_step_scale()));
}

TEST_CASE("a gate composes with every op, not only with subtract") {
    // The reason a gate lives on the combine record rather than in the mode
    // enum: masking has to gate a boolean, and a boolean is a mode. If it were
    // a mode it could not also gate an add or a smooth union.
    //
    // Each op gets its OWN probe, because they act in complementary places and
    // a single point cannot show all three. Adding a thin bar INSIDE a ball
    // changes nothing there — the union is still the ball — so an add has to be
    // probed where it puts material the ball does not have. The first draft of
    // this case used one probe for all three and passed for subtract and
    // intersect while proving nothing about add.
    auto with_op = [](scene::Op op, bool gated, float k = 0.0f) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node body;
        body.prim = scene::Prim::sphere(kBody);
        l.sdf->insert(body);
        scene::Node other;
        // Longer than the body, so an add reaches outside it.
        other.prim = scene::Prim::box(cf3(3.0f, 0.25f, 0.25f));
        other.op = op;
        if (k > 0.0f) other.blend = scene::Blend{scene::BlendProfile::Quadratic, k};
        if (gated) {
            other.gate = half_gate();
            other.gate_width = 0.15f;
        }
        l.sdf->insert(other);
        return scene::compile_document(doc);
    };

    const scene::Tape solid = scene::compile_document(bare_body());
    struct Case {
        scene::Op op;
        cfloat3 probe;
        const char* why;
    };
    const Case cases[] = {
        {scene::Op::Subtract, cf3(0.7f, 0, 0), "inside both: the bar carves the ball"},
        {scene::Op::Add, cf3(1.2f, 0, 0), "outside the ball, inside the bar: the bar adds"},
        {scene::Op::Intersect, cf3(0.7f, 0.5f, 0), "inside the ball, outside the bar: it is cut"},
    };
    for (const Case& c : cases) {
        const scene::Tape open = with_op(c.op, false);
        const scene::Tape shut = with_op(c.op, true);
        CAPTURE(c.why);
        // The fixture is meaningful: the UNGATED form changes something here.
        REQUIRE(open.eval(c.probe).d != solid.eval(c.probe).d);
        // ...and inside the protected region the op did not happen at all.
        REQUIRE(shut.eval(c.probe).d == solid.eval(c.probe).d);
    }

    // ...including a SMOOTH union, where the item reaches past its own surface.
    const cfloat3 outside = cf3(1.2f, 0, 0);
    const scene::Tape smooth_open = with_op(scene::Op::Add, false, 0.2f);
    const scene::Tape smooth_shut = with_op(scene::Op::Add, true, 0.2f);
    CHECK(smooth_open.eval(outside).d != solid.eval(outside).d);
    CHECK(smooth_shut.eval(outside).d == solid.eval(outside).d);
}

TEST_CASE("gating does not widen an item's influence, so culling is unchanged") {
    // The claim the proposal makes and this checks rather than assumes: a gate
    // only ever REMOVES an item's effect, so the influence bound computed for
    // the ungated item stays a superset and per-brick culling needs no change.
    //
    // A formulation that could GROW a bound would be the dangerous one — the
    // cull would drop an item that still reaches the brick, and the result is
    // missing geometry that only shows up at some camera angles.
    const scene::Document ungated_doc = body_with_channel(nullptr);
    const scene::Document gated_doc = body_with_channel(half_gate(), 0.2f);

    const scene::Tape ungated = scene::compile_document(ungated_doc);
    const scene::Tape gated = scene::compile_document(gated_doc);

    // The tape's own bound — the union of item influence bounds, which is what
    // raycast clipping and the cull index both work from.
    CHECK(gated.bounds.min.x == doctest::Approx(ungated.bounds.min.x));
    CHECK(gated.bounds.min.y == doctest::Approx(ungated.bounds.min.y));
    CHECK(gated.bounds.min.z == doctest::Approx(ungated.bounds.min.z));
    CHECK(gated.bounds.max.x == doctest::Approx(ungated.bounds.max.x));
    CHECK(gated.bounds.max.y == doctest::Approx(ungated.bounds.max.y));
    CHECK(gated.bounds.max.z == doctest::Approx(ungated.bounds.max.z));

    // ...and the stronger claim, checked where it actually matters: compiling
    // against a cull region must not drop anything the gated item still
    // affects. Compile a region around the OPEN side, where the subtract does
    // act, and require it to agree with the uncculled tape there.
    const scene::CullRegion region{math::Aabb{cf3(-1.4f, -0.4f, -0.4f), cf3(-0.2f, 0.4f, 0.4f)}};
    const scene::Tape culled = scene::compile_document(gated_doc, &region);
    for (int i = 0; i < 64; ++i) {
        const cfloat3 p = cf3(-1.2f + static_cast<float>(i % 4) * 0.25f,
                              static_cast<float>((i / 4) % 4 - 2) * 0.08f,
                              static_cast<float>((i / 16) % 4 - 2) * 0.08f);
        REQUIRE(culled.eval(p).d == gated.eval(p).d);
    }
}

// -- the gate's frame (issue #394) -------------------------------------------
//
// A gate is WORLD-ADDRESSED. `voxel::MaskField` is stored in world units on its
// own lattice deliberately, and `brush::mask_to_field` measures it there, so
// the protected region is where the artist painted it and stays there however
// the gated item is placed.
//
// Until 0.67.0 the tape placed the gate volume by `layer.xform * item.xform`,
// which moved the protected region by the transform of the very item it was
// meant to hold back. From outside that is indistinguishable from a gate that
// does nothing: #394 was filed as "accepted and inert" by a host whose cuts all
// carried a placement.
//
// Every fixture above uses an identity item transform, which is exactly why
// none of them caught it. These place the item and keep what it carves the
// same, so a difference in the result can only be the gate moving.

namespace {

// The channel again, with the cut PLACED. Its box is half-extent 2.0 along X
// through a unit ball, so sliding it along that axis, turning it about Y, or
// growing it removes the same material either way — anything these tests see
// is the gate, not the carve.
scene::Document placed_channel(std::shared_ptr<const FieldVolume> gate,
                               const math::Transform& cut_xform,
                               const math::Transform& layer_xform = math::Transform::identity()) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.xform = layer_xform;
    scene::Node body;
    body.prim = scene::Prim::sphere(kBody);
    l.sdf->insert(body);
    scene::Node cut;
    cut.prim = scene::Prim::box(cf3(2.0f, 0.25f, 0.25f));
    cut.op = scene::Op::Subtract;
    cut.xform = cut_xform;
    if (gate) {
        cut.gate = std::move(gate);
        cut.gate_width = 0.1f;
    }
    l.sdf->insert(cut);
    return doc;
}

// Probes chosen so a displaced gate cannot land on them by luck: 0.3 is inside
// the painted region but within 0.5 of its border, which is how far every
// placement below moves the gate under the old behaviour.
constexpr float kInside = 0.3f;
constexpr float kOutside = -0.7f;

// The claim, for one placement: the gate protects the same world region it did
// at identity, and the carve really was invariant so that means something.
void check_gate_is_world_fixed(const math::Transform& cut_xform,
                               const math::Transform& layer_xform = math::Transform::identity()) {
    const scene::Tape rest = scene::compile_document(placed_channel(nullptr, {}, {}));
    const scene::Tape moved = scene::compile_document(placed_channel(nullptr, cut_xform, layer_xform));
    const scene::Tape gated = scene::compile_document(placed_channel(half_gate(), cut_xform, layer_xform));
    const scene::Tape solid = scene::compile_document(bare_body());

    for (const cfloat3 p : {cf3(kInside, 0, 0), cf3(kOutside, 0, 0)}) {
        // The fixture's own claim first: this placement changes nothing about
        // what the ungated cut removes. Without this the rest proves nothing.
        CHECK(moved.eval(p).d == doctest::Approx(rest.eval(p).d).epsilon(1e-5));
        CHECK(moved.eval(p).d > 0.0f);  // carved away when nothing gates it
    }
    // Protected where the mask was painted...
    CHECK(gated.eval(cf3(kInside, 0, 0)).d == doctest::Approx(solid.eval(cf3(kInside, 0, 0)).d));
    // ...and open where it was not.
    CHECK(gated.eval(cf3(kOutside, 0, 0)).d ==
          doctest::Approx(moved.eval(cf3(kOutside, 0, 0)).d));
}

}  // namespace

TEST_CASE("a gate stays where the mask was painted when the item is moved") {
    // Slid half a unit along its own long axis: the same channel, a gate that
    // used to slide with it and protect half a unit further out.
    check_gate_is_world_fixed(math::Transform{cf3(0.5f, 0, 0), math::Quat::identity(), 1.0f});
}

TEST_CASE("a gate does not turn with the item") {
    // A box symmetric about the origin, turned onto its own footprint. The cut
    // is bit-for-bit the same; the gate used to end up on the other side of the
    // body, protecting the half the artist opened and opening the half they
    // protected.
    const math::Quat half_turn = math::Quat::from_axis_angle(cf3(0, 1, 0), 3.14159265f);
    check_gate_is_world_fixed(math::Transform{cf3(0, 0, 0), half_turn, 1.0f});
}

TEST_CASE("a gate does not turn with the layer either") {
    // The layer's transform reached the gate by the same route. A unit ball and
    // an origin-centred box are both invariant under this turn, so the document
    // renders identically and only the gate could move.
    const math::Quat half_turn = math::Quat::from_axis_angle(cf3(0, 1, 0), 3.14159265f);
    check_gate_is_world_fixed(math::Transform::identity(),
                              math::Transform{cf3(0, 0, 0), half_turn, 1.0f});
}

TEST_CASE("a gate does not scale with the item") {
    // Growing the cut grows what it removes, so this one cannot use the shared
    // helper's invariance check — but the protected region must not grow with
    // it. At five times, the old behaviour pushed the mask's border from 0.1
    // out to 0.5 and left the probe at 0.3 unprotected.
    const math::Transform big{cf3(0, 0, 0), math::Quat::identity(), 5.0f};
    const scene::Tape ungated = scene::compile_document(placed_channel(nullptr, big));
    const scene::Tape gated = scene::compile_document(placed_channel(half_gate(), big));
    const scene::Tape solid = scene::compile_document(bare_body());

    const cfloat3 inside = cf3(kInside, 0, 0);
    CHECK(ungated.eval(inside).d > 0.0f);  // the bigger cut still reaches here
    CHECK(gated.eval(inside).d == doctest::Approx(solid.eval(inside).d));

    const cfloat3 outside = cf3(kOutside, 0, 0);
    CHECK(gated.eval(outside).d == doctest::Approx(ungated.eval(outside).d));
}

TEST_CASE("a gate's declared cost is charged in world units, not the layer's") {
    // `gate_width` is in world units and the gate is now read in world space,
    // so the layer's scale must not multiply it. It used to, and in the
    // dangerous direction: a WIDER gate costs less, so a layer scaled up
    // declared a step scale it had not earned and a marcher stepping by it
    // would punch through the masked boundary.
    //
    // Both documents describe the SAME world: the layer scales up by four and
    // every item scales down by the same four, so the geometry, the gate and
    // the width are identical and only the number the compiler charges can
    // differ.
    auto build = [](float layer_scale) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.xform.scale = layer_scale;
        const float item_scale = 1.0f / layer_scale;
        scene::Node body;
        body.prim = scene::Prim::sphere(kBody);
        body.xform.scale = item_scale;
        l.sdf->insert(body);
        scene::Node cut;
        cut.prim = scene::Prim::box(cf3(2.0f, 0.25f, 0.25f));
        cut.op = scene::Op::Subtract;
        cut.xform.scale = item_scale;
        cut.gate = half_gate();
        cut.gate_width = 0.15f;
        l.sdf->insert(cut);
        return scene::compile_document(doc);
    };
    const scene::Tape at_one = build(1.0f);
    const scene::Tape at_four = build(4.0f);

    // Same world, so the same field...
    for (const cfloat3 p : {cf3(0.3f, 0, 0), cf3(-0.7f, 0, 0), cf3(0, 0.9f, 0)})
        CHECK(at_four.eval(p).d == doctest::Approx(at_one.eval(p).d).epsilon(1e-5));
    // ...and the same declared cost for it.
    CHECK(at_four.safe_step_scale() == doctest::Approx(at_one.safe_step_scale()).epsilon(1e-4));

    // And the bound is honest: marching the scaled document by its own number
    // reaches the surface without stepping through it.
    const float scale = at_four.safe_step_scale();
    REQUIRE(scale > 0.0f);
    for (int i = 0; i < 16; ++i) {
        const float y = (static_cast<float>(i % 4) - 1.5f) * 0.12f;
        const float z = (static_cast<float>(i / 4) - 1.5f) * 0.12f;
        float travelled = 0.0f;
        for (int step = 0; step < 4000 && travelled < 6.0f; ++step) {
            const float d = at_four.eval(cf3(-3.0f + travelled, y, z)).d;
            if (d <= 1e-4f) break;
            const float advance = scale * d;
            CAPTURE(i);
            CAPTURE(travelled);
            REQUIRE(at_four.eval(cf3(-3.0f + travelled + advance, y, z)).d >= -1e-3f);
            travelled += advance;
        }
    }
}
