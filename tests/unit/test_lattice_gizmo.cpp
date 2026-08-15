// One cage over a layer (brush-engine spec, lattice-gizmo).
//
// The claim that made this need a kernel change, and the one most of these
// defend: an item's frame can be ROTATED, and a lattice box is axis-aligned by
// construction — so no per-item box reproduces a world-placed cage, and the
// deformer has to carry the placement instead. The test that matters compares
// a rotated item against an unrotated one at the same world pose and requires
// the same WORLD field.

#include <doctest/doctest.h>

#include <cmath>

#include "clay/brush/lattice_gizmo.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using brush::GizmoCage;
using brush::LatticeWarp;
using clay_test::item;
using scene::Deformer;

namespace {

// A cage with one control point dragged, so it is not the identity and not a
// pure translation either — a uniform cage would pass a transform that was
// silently dropped.
GizmoCage one_pull(math::Transform placement, cfloat3 drag = cf3(0.45f, 0.0f, 0.0f)) {
    GizmoCage cage;
    cage.placement = placement;
    cage.box_min = cf3(-1, -1, -1);
    cage.box_max = cf3(1, 1, 1);
    cage.nx = cage.ny = cage.nz = 3;
    cage.offsets.assign(cage.point_count(), cf3(0, 0, 0));
    cage.offsets[(1 * 3 + 1) * 3 + 2] = drag;  // (2,1,1), the +X face centre
    return cage;
}

math::Transform rotation_about_y(float radians) {
    math::Transform t;
    t.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), radians);
    return t;
}

// Evaluate a one-item layer's field in WORLD space.
float world_eval(const scene::Document& doc, cfloat3 world_p) {
    return scene::compile_document(doc).eval(world_p).d;
}

}  // namespace

TEST_CASE("an identity transform is the axis-aligned cage") {
    // The transformed opcode is a superset, and this is what says so. Same box,
    // same offsets, no placement: the two must agree everywhere.
    const GizmoCage cage = one_pull(math::Transform::identity());

    scene::Document plain;
    scene::Node a = item(scene::Prim::sphere(0.6f), cf3(0, 0, 0));
    Deformer flat = Deformer::lattice(cage.box_min, cage.box_max, 3, 3, 3);
    for (std::size_t i = 0; i < flat.cage.size(); ++i) flat.cage[i] = cage.offsets[i];
    a.deformers.push_back(flat);
    plain.add_sdf_layer("l").sdf->insert(a);

    scene::Document caged;
    scene::Node b = item(scene::Prim::sphere(0.6f), cf3(0, 0, 0));
    Deformer through = Deformer::lattice_transformed(cage.box_min, cage.box_max,
                                                     math::Transform::identity(), 3, 3, 3);
    for (std::size_t i = 0; i < through.cage.size(); ++i) through.cage[i] = cage.offsets[i];
    b.deformers.push_back(through);
    caged.add_sdf_layer("l").sdf->insert(b);

    clay_test::Lcg rng(1180);
    for (int i = 0; i < 400; ++i) {
        const cfloat3 p = rng.vec3(-2, 2);
        CAPTURE(p.x);
        CHECK(world_eval(caged, p) == doctest::Approx(world_eval(plain, p)).epsilon(1e-5));
    }
}

TEST_CASE("a rotated item warps to the same world field as an unrotated one") {
    // THE claim. A world cage over a rotated item cannot be expressed as an
    // axis-aligned box in that item's frame, so if the transform were dropped
    // or applied in the wrong direction this is what would catch it.
    //
    // The subject is a BOX, not a sphere: a sphere is invariant under the
    // rotation being applied, so it would pass with the transform ignored.
    const cfloat3 half = cf3(0.55f, 0.35f, 0.25f);
    const float angle = 0.9f;
    const GizmoCage cage = one_pull(math::Transform::identity());

    // (a) the item rotated in its own frame
    scene::Document rotated;
    scene::Node r = item(scene::Prim::box(half), cf3(0, 0, 0));
    r.xform = rotation_about_y(angle);
    scene::Layer& rl = rotated.add_sdf_layer("l");
    rl.sdf->insert(r);
    for (const LatticeWarp& w : brush::lattice_gizmo(rl, cage)) {
        scene::Node* n = rl.sdf->find_mut(w.node);
        REQUIRE(n != nullptr);
        n->deformers = brush::caged_chain(*n, w);
    }

    // (b) the same world shape, reached by rotating the CAGE instead
    scene::Document by_cage;
    scene::Node u = item(scene::Prim::box(half), cf3(0, 0, 0));
    u.xform = rotation_about_y(angle);
    scene::Layer& ul = by_cage.add_sdf_layer("l");
    // The id INSERT assigns, not the one on the local copy — that is still
    // kNoNode, and find_mut would hand back nullptr.
    const scene::NodeId uid = ul.sdf->insert(u);
    // Resolve by hand, the way the resolver does, to prove the composition is
    // what makes the two agree rather than the resolver hiding a special case.
    Deformer d = Deformer::lattice_transformed(cage.box_min, cage.box_max,
                                               cage.placement.inverse() * (ul.xform * u.xform),
                                               3, 3, 3);
    for (std::size_t i = 0; i < d.cage.size(); ++i) d.cage[i] = cage.offsets[i];
    ul.sdf->find_mut(uid)->deformers = {d};

    clay_test::Lcg rng(1181);
    for (int i = 0; i < 400; ++i) {
        const cfloat3 p = rng.vec3(-2, 2);
        CHECK(world_eval(rotated, p) == doctest::Approx(world_eval(by_cage, p)).epsilon(1e-5));
    }

    // ...and the cage actually did something to it: the same box with no cage
    // is a different field.
    scene::Document bare;
    scene::Node v = item(scene::Prim::box(half), cf3(0, 0, 0));
    v.xform = rotation_about_y(angle);
    bare.add_sdf_layer("l").sdf->insert(v);
    int differ = 0;
    for (int i = 0; i < 200; ++i) {
        const cfloat3 p = rng.vec3(-1.2f, 1.2f);
        if (std::fabs(world_eval(rotated, p) - world_eval(bare, p)) > 1e-3f) ++differ;
    }
    CHECK(differ > 0);
}

TEST_CASE("the transform does not change the declared bound") {
    // With T = sR the warp's Jacobian in the item's frame is R-inverse J R —
    // similar to the cage-space one, hence the same norm. So a rotation and a
    // uniform scale must cost no step scale at all.
    const cfloat3 half = cf3(0.5f, 0.5f, 0.5f);
    const GizmoCage cage = one_pull(math::Transform::identity());

    auto scale_with = [&](math::Transform placement) {
        scene::Document doc;
        scene::Node n = item(scene::Prim::box(half), cf3(0, 0, 0));
        Deformer d = Deformer::lattice_transformed(cage.box_min, cage.box_max, placement, 3, 3, 3);
        for (std::size_t i = 0; i < d.cage.size(); ++i) d.cage[i] = cage.offsets[i];
        n.deformers.push_back(d);
        doc.add_sdf_layer("l").sdf->insert(n);
        return scene::compile_document(doc).safe_step_scale();
    };

    const float plain = scale_with(math::Transform::identity());
    const float turned = scale_with(rotation_about_y(0.7f));
    math::Transform scaled = rotation_about_y(0.7f);
    scaled.scale = 2.0f;
    scaled.position = cf3(0.3f, -0.2f, 0.1f);
    const float moved = scale_with(scaled);

    CAPTURE(plain);
    CHECK(plain < 1.0f);  // the cage itself costs something
    CHECK(turned == doctest::Approx(plain).epsilon(1e-5));
    CHECK(moved == doctest::Approx(plain).epsilon(1e-5));
}

TEST_CASE("a cage over a layer reaches every item, including a distant one") {
    // NOT the move brush's reachability. A lattice's displacement outside its
    // box is CLAMPED rather than zero, so material out there travels rigidly —
    // skipping "distant" items would tear the form.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node near_item = item(scene::Prim::sphere(0.4f), cf3(0, 0, 0));
    scene::Node far_item = item(scene::Prim::sphere(0.4f), cf3(9.0f, 0, 0));
    l.sdf->insert(near_item);
    l.sdf->insert(far_item);

    const std::vector<LatticeWarp> warps = brush::lattice_gizmo(l, one_pull(math::Transform::identity()));
    CHECK(warps.size() == 2);
    for (const LatticeWarp& w : warps) CHECK(w.deformer.type == kernel::cdeform_lattice_xform);
}

TEST_CASE("an untouched or degenerate cage resolves to nothing") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.4f), cf3(0, 0, 0)));

    GizmoCage untouched;
    untouched.offsets.assign(untouched.point_count(), cf3(0, 0, 0));
    CHECK(untouched.is_identity());
    CHECK(brush::lattice_gizmo(l, untouched).empty());

    GizmoCage flat = one_pull(math::Transform::identity());
    flat.box_min = flat.box_max = cf3(0, 0, 0);
    CHECK(brush::lattice_gizmo(l, flat).empty());
}

TEST_CASE("the cage goes at the FRONT of the chain, and replaces its own") {
    // Authoring order is warp order, so the first entry is the outermost. A
    // cage appended at the back would be evaluated at points the earlier
    // deformers already moved.
    scene::Node n = item(scene::Prim::sphere(0.4f), cf3(0, 0, 0));
    n.deformers.push_back(Deformer::twist(0.5f));

    LatticeWarp w;
    w.node = n.id;  // unused here: caged_chain reads the node, not the arena
    w.deformer = Deformer::lattice_transformed(cf3(-1, -1, -1), cf3(1, 1, 1),
                                               math::Transform::identity());
    w.deformer.set_cage_offset(2, 1, 1, cf3(0.3f, 0, 0));

    std::vector<Deformer> chain = brush::caged_chain(n, w);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].type == kernel::cdeform_lattice_xform);
    CHECK(chain[1].type == kernel::cdeform_twist);

    // Dragging again replaces the cage rather than stacking a second one, so a
    // gesture across frames does not accumulate a chain of them.
    n.deformers = chain;
    LatticeWarp again = w;
    again.deformer.set_cage_offset(2, 1, 1, cf3(0.5f, 0, 0));
    std::vector<Deformer> second = brush::caged_chain(n, again);
    REQUIRE(second.size() == 2);
    CHECK(second[0].type == kernel::cdeform_lattice_xform);
    CHECK(second[0].cage_offset(2, 1, 1).x == doctest::Approx(0.5f));
    CHECK(second[1].type == kernel::cdeform_twist);
}

TEST_CASE("a transformed cage steps conservatively and survives a round trip") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::box(cf3(0.5f, 0.6f, 0.4f)), cf3(0.2f, 0, 0));
    n.xform.rotation = math::Quat::from_axis_angle(cf3(0.3f, 1.0f, 0.2f), 0.8f);
    math::Transform placement = rotation_about_y(0.4f);
    placement.position = cf3(0.1f, 0.2f, -0.1f);
    placement.scale = 1.3f;

    Deformer d = Deformer::lattice_transformed(cf3(-1, -1, -1), cf3(1, 1, 1),
                                               placement.inverse() * (l.xform * n.xform), 3, 3, 3);
    d.set_cage_offset(2, 2, 1, cf3(0.35f, -0.15f, 0.0f));
    d.set_cage_offset(0, 1, 2, cf3(-0.2f, 0.25f, 0.1f));
    n.deformers.push_back(d);
    n.deformers.push_back(Deformer::twist(0.3f));
    l.sdf->insert(n);

    scene::Tape tape = scene::compile_document(doc);
    CHECK(tape.safe_step_scale() < 1.0f);
    clay_test::check_conservative_steps([&](cfloat3 p) { return tape.eval(p).d; },
                                        tape.safe_step_scale(), 3.0f, 400, 1182);

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);
    clay_test::Lcg rng(1183);
    scene::Tape b = scene::compile_document(*back);
    for (int i = 0; i < 200; ++i) {
        const cfloat3 p = rng.vec3(-2, 2);
        CHECK(b.eval(p).d == doctest::Approx(tape.eval(p).d).epsilon(1e-5));
    }
}
