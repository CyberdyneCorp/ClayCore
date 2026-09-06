// THE PARITY FIXTURE, and the bounds and field-info halves of the layer fold
// (fold-the-layers-with-an-operator, scene-model tasks 3.1-3.3).
//
// The claim this file defends is the one the whole change stands on: a shape
// expressed as layer A with layer B composed over it, and the same shape
// expressed as ONE layer holding A and then B, are the SAME DOCUMENT. Not
// approximately, and not only in distance -- in colour, in the geometric extent
// meshing and raycasting clip against, and in the safe step a marcher is told
// it may take. A layer boolean that agreed with an item boolean in distance and
// disagreed in any of the other three would be a second evaluator wearing the
// first one's answers, and each of those three fails silently in its own way:
// a wrong colour renders, a bound too small renders as MISSING SURFACE rather
// than as an error, and a safe step that is too large is a marcher walking
// through the surface it was looking for.
//
// So every comparison here is between two documents built by one function that
// cannot drift, and every one of them is paired with a check that the two sides
// were not trivially equal to begin with.
//
// WHAT IS NOT HERE. The symbolic fold itself and the first-visible-layer rule
// (test_layer_fold.cpp, tasks 2.x); the four resumable-compile sites and the
// brick refill's own fold (tasks 4.x). Note in particular that a RESUMED
// compile does not yet reproduce the ring `fold_layer_bounds` adds, because
// TapeCheckpoint carries no extent -- that is task 4.4 and there is nothing
// here that would catch it.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/kernel/exactness.h"
#include "clay/scene/bounds.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::scene;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// The regular lattice every bit-identity comparison in this repo samples over.
std::vector<cfloat3> lattice(int n, float half = 1.6f) {
    std::vector<cfloat3> pts;
    pts.reserve(static_cast<std::size_t>(n) * n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                auto a = [n, half](int v) {
                    return -half + 2.0f * half * static_cast<float>(v) / (n - 1);
                };
                pts.push_back(cf3(a(i), a(j), a(k)));
            }
    return pts;
}

// Distance AND colour: `ctape_combine_values` couples the two for a union and
// carries the accumulator's through for a carve, so a fold that picks the right
// shape and the wrong colour is a real and separate failure.
std::vector<float> sample(const Tape& t, const std::vector<cfloat3>& pts) {
    std::vector<float> out;
    out.reserve(pts.size() * 4);
    for (cfloat3 p : pts) {
        const kernel::CTapeValue v = t.eval(p);
        out.push_back(v.d);
        out.push_back(v.color.x);
        out.push_back(v.color.y);
        out.push_back(v.color.z);
    }
    return out;
}

// A COUNT of differing samples, never the vectors themselves: doctest
// stringifies the operands of a failing CHECK and a lattice of 4,096 points
// prints a megabyte of floats nobody reads.
int differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++n;
    return n;
}

Node sphere_at(cfloat3 pos, float r, cfloat3 color) {
    Node n;
    n.prim = Prim::sphere(r);
    n.xform.position = pos;
    n.color = color;
    return n;
}

LayerComposition composed(Op op, BlendProfile profile = BlendProfile::Hard, float k = 0.0f,
                          float rounding = 0.0f) {
    LayerComposition c;
    c.op = op;
    c.blend.profile = profile;
    c.blend.k = k;
    c.rounding = rounding;
    return c;
}

// -- the two forms of one document -------------------------------------------

// THE SPEC'S OWN SCENARIO, literally: one item per layer, against one layer
// holding the two items with the composition spelled on the SECOND ITEM.
//
// The composition carries no rounding here and must not: a LAYER's rounding is
// the combine's rb alone, exactly as a GROUP's is, while an ITEM's rounding
// ALSO rounds its own primitive. The two are the same number in different
// places and comparing across them would be comparing two shapes.
Document one_item_each(bool two_layers, const LayerComposition& comp) {
    const Node base = sphere_at(cf3(0, 0, 0), 1.0f, cf3(0.8f, 0.2f, 0.2f));
    Node cut = sphere_at(cf3(0.7f, 0.1f, 0.0f), 0.6f, cf3(0.2f, 0.3f, 0.9f));
    Document doc;
    if (two_layers) {
        doc.add_sdf_layer("base").sdf->insert(base);
        Layer& over = doc.add_sdf_layer("over");
        over.sdf->insert(cut);  // FIRST in its own chain, so its item op is Add
        over.composition = comp;
        return doc;
    }
    Layer& only = doc.add_sdf_layer("only");
    only.sdf->insert(base);
    cut.op = comp.op;
    cut.blend = comp.blend;
    only.sdf->insert(cut);
    return doc;
}

// The general form. A layer holds a CHAIN, so the one-layer equivalent of a
// composed layer of several items is a GROUP carrying the composition -- an
// item chain A, B(Subtract), C(Subtract) subtracts twice where the layer form
// unions B with C first and subtracts once. A group's rounding is its combine's
// rb, the same as a layer's, so this form can carry one.
Document several_items(bool two_layers, const LayerComposition& comp) {
    const Node b1 = sphere_at(cf3(0, 0, 0), 1.0f, cf3(0.8f, 0.2f, 0.2f));
    const Node b2 = sphere_at(cf3(0, 0.9f, 0), 0.5f, cf3(0.9f, 0.7f, 0.1f));
    const Node c1 = sphere_at(cf3(0.7f, 0.1f, 0.0f), 0.6f, cf3(0.2f, 0.3f, 0.9f));
    const Node c2 = sphere_at(cf3(0.6f, 0.0f, 0.6f), 0.4f, cf3(0.1f, 0.8f, 0.4f));
    Document doc;
    if (two_layers) {
        Layer& base = doc.add_sdf_layer("base");
        base.sdf->insert(b1);
        base.sdf->insert(b2);
        Layer& over = doc.add_sdf_layer("over");
        over.sdf->insert(c1);
        over.sdf->insert(c2);
        over.composition = comp;
        return doc;
    }
    Layer& only = doc.add_sdf_layer("only");
    only.sdf->insert(b1);
    only.sdf->insert(b2);
    Node group;
    group.is_group = true;
    group.op = comp.op;
    group.blend = comp.blend;
    group.rounding = comp.rounding;
    const NodeId gid = only.sdf->insert(group);
    only.sdf->insert(c1, gid);
    only.sdf->insert(c2, gid);
    return doc;
}

// -- what "the same document" means, in four parts ---------------------------

void same_field(const Tape& a, const Tape& b, const std::vector<cfloat3>& pts) {
    CHECK(differing(sample(a, pts), sample(b, pts)) == 0);
}

void same_info(const Tape& a, const Tape& b) {
    CHECK(a.info.is_exact == b.info.is_exact);
    CHECK(a.info.lipschitz == doctest::Approx(b.info.lipschitz));
    CHECK(a.safe_step_scale() == doctest::Approx(b.safe_step_scale()));
    CHECK(a.lipschitz_bounds_gradient == b.lipschitz_bounds_gradient);
}

void same_bounds(const Tape& a, const Tape& b) {
    CHECK(a.bounds.min.x == doctest::Approx(b.bounds.min.x));
    CHECK(a.bounds.min.y == doctest::Approx(b.bounds.min.y));
    CHECK(a.bounds.min.z == doctest::Approx(b.bounds.min.z));
    CHECK(a.bounds.max.x == doctest::Approx(b.bounds.max.x));
    CHECK(a.bounds.max.y == doctest::Approx(b.bounds.max.y));
    CHECK(a.bounds.max.z == doctest::Approx(b.bounds.max.z));
}

// Whether `box` holds every point of `pts` where the field has material, plus
// the surface's own side of it. A bound is only ever wrong by being too small,
// so this is the assertion that matters and it is made on the FIELD rather
// than on the arithmetic that produced the box.
int material_outside(const Tape& t, const std::vector<cfloat3>& pts, const math::Aabb& box) {
    int out = 0;
    for (cfloat3 p : pts)
        if (t.eval(p).d <= 0.0f && !box.contains(p)) ++out;
    return out;
}

}  // namespace

// -- 3.3: THE PARITY FIXTURE -------------------------------------------------

TEST_CASE("layer parity: two layers and one layer of two items are one document") {
    const std::vector<cfloat3> pts = lattice(16);
    // The teeth for every subcase: the two-layer union arm, which every other
    // composition has to differ from. Without this a fold that ignored the
    // composition entirely would pass the whole case.
    const std::vector<float> unioned =
        sample(compile_document(one_item_each(true, composed(Op::Add))), pts);

    LayerComposition comp;
    bool expect_difference = true;
    SUBCASE("hard union") {
        comp = composed(Op::Add);
        expect_difference = false;  // this IS the union arm
    }
    SUBCASE("smooth union") { comp = composed(Op::Add, BlendProfile::Quadratic, 0.25f); }
    SUBCASE("subtract") { comp = composed(Op::Subtract); }
    SUBCASE("smooth subtract") { comp = composed(Op::Subtract, BlendProfile::Quadratic, 0.25f); }
    SUBCASE("intersect") { comp = composed(Op::Intersect); }
    SUBCASE("smooth intersect") { comp = composed(Op::Intersect, BlendProfile::Cubic, 0.2f); }
    SUBCASE("chamfered union") { comp = composed(Op::Add, BlendProfile::Chamfer, 0.3f); }
    SUBCASE("paint") { comp = composed(Op::Paint, BlendProfile::Quadratic, 0.3f); }

    const Tape two = compile_document(one_item_each(true, comp));
    const Tape one = compile_document(one_item_each(false, comp));

    const std::vector<float> two_s = sample(two, pts);
    const bool differs_from_union = differing(two_s, unioned) > 0;
    CHECK(differs_from_union == expect_difference);

    same_field(two, one, pts);
    same_bounds(two, one);
    same_info(two, one);
    // And the tapes are the same tape: the layer fold emits the combine an item
    // boolean emits, through the same emitter, with the same parameters.
    CHECK(two.instrs.size() == one.instrs.size());
    const bool same_params = two.params == one.params;
    CHECK(same_params);
}

TEST_CASE("layer parity: a composed layer of several items is one group of them") {
    const std::vector<cfloat3> pts = lattice(16);
    const std::vector<float> unioned =
        sample(compile_document(several_items(true, composed(Op::Add))), pts);

    SUBCASE("a fold with no support of its own agrees in field, bounds and safe step") {
        LayerComposition comp;
        SUBCASE("union") { comp = composed(Op::Add); }
        SUBCASE("subtract") { comp = composed(Op::Subtract); }
        SUBCASE("intersect") { comp = composed(Op::Intersect); }

        const Tape two = compile_document(several_items(true, comp));
        const Tape one = compile_document(several_items(false, comp));
        CHECK(layer_blend_support(several_items(true, comp).layers.back()) == 0.0f);
        same_field(two, one, pts);
        same_info(two, one);
        same_bounds(two, one);
    }

    SUBCASE("the cutter really reaches these samples") {
        const Tape two = compile_document(several_items(true, composed(Op::Subtract)));
        CHECK(differing(sample(two, pts), unioned) > 0);
    }

    SUBCASE("a fold WITH support agrees in field and safe step, and is wider by its own ring") {
        // The one place the two forms part company, and it is the GROUP path's
        // gap rather than the layer fold's: a group adds no ring for its own
        // combine's support, so its box is the plain union of its children
        // while the layer fold's is that union dilated. See fold_layer_bounds
        // in tape_build.cpp for why closing it is the resumable checkpoint's
        // problem and not this change's. The layer's box CONTAINS the group's,
        // which is the safe direction, and this pins that the difference is
        // exactly one ring and not something else.
        LayerComposition comp;
        SUBCASE("smooth union") { comp = composed(Op::Add, BlendProfile::Quadratic, 0.3f); }
        SUBCASE("groove") { comp = composed(Op::Groove, BlendProfile::Hard, 0.15f, 0.1f); }
        SUBCASE("shell") { comp = composed(Op::Shell, BlendProfile::Hard, 0.12f); }
        SUBCASE("incise") { comp = composed(Op::Incise, BlendProfile::Hard, 0.2f, 0.25f); }

        const Tape two = compile_document(several_items(true, comp));
        const Tape one = compile_document(several_items(false, comp));
        same_field(two, one, pts);
        same_info(two, one);

        const float ring = layer_blend_support(several_items(true, comp).layers.back());
        CHECK(ring > 0.0f);
        const float slack = ring + 1e-5f;
        CHECK(one.bounds.min.x - two.bounds.min.x >= 0.0f);
        CHECK(one.bounds.min.x - two.bounds.min.x <= slack);
        CHECK(one.bounds.min.y - two.bounds.min.y >= 0.0f);
        CHECK(one.bounds.min.y - two.bounds.min.y <= slack);
        CHECK(one.bounds.min.z - two.bounds.min.z >= 0.0f);
        CHECK(one.bounds.min.z - two.bounds.min.z <= slack);
        CHECK(two.bounds.max.x - one.bounds.max.x >= 0.0f);
        CHECK(two.bounds.max.x - one.bounds.max.x <= slack);
        CHECK(two.bounds.max.y - one.bounds.max.y >= 0.0f);
        CHECK(two.bounds.max.y - one.bounds.max.y <= slack);
        CHECK(two.bounds.max.z - one.bounds.max.z >= 0.0f);
        CHECK(two.bounds.max.z - one.bounds.max.z <= slack);
    }
}

// -- 3.1: BOUNDS -------------------------------------------------------------

namespace {

// TWO BOXES THAT ABUT, so their shared edge sits ON the union box's face. A
// smooth union bridges across that edge and the bridge's surface stands proud
// of BOTH boxes -- the only arrangement in which a combine's bulge escapes the
// union of the operands' bounds, and the only one that can tell a missing ring
// from a decorative one.
Document abutting_slabs(const LayerComposition& comp) {
    Document doc;
    Node left;
    left.prim = Prim::box(cf3(0.5f, 0.1f, 0.1f));
    left.xform.position = cf3(-0.5f, 0, 0);
    doc.add_sdf_layer("left").sdf->insert(left);

    Node right;
    right.prim = Prim::box(cf3(0.5f, 0.1f, 0.1f));
    right.xform.position = cf3(0.5f, 0, 0);
    Layer& over = doc.add_sdf_layer("right");
    over.sdf->insert(right);
    over.composition = comp;
    return doc;
}

}  // namespace

TEST_CASE("layer bounds: a smooth fold's bulge is inside the box, and only because of the ring") {
    const LayerComposition smooth = composed(Op::Add, BlendProfile::Quadratic, 0.2f);
    const Tape blended = compile_document(abutting_slabs(smooth));
    // The same geometry folded hard: identical items, so its box is the plain
    // union of the item bounds -- which is exactly what tape.bounds was before
    // a layer could carry a combine at all.
    const Tape hard = compile_document(abutting_slabs(composed(Op::Add)));

    // A lattice fine enough to land in the bridge above the shared edge.
    const std::vector<cfloat3> pts = lattice(61, 0.75f);

    CHECK(material_outside(blended, pts, blended.bounds) == 0);
    // ...and the ring is load-bearing: the same material IS outside the box the
    // fold would have reported without it. That is missing surface in a mesh
    // and a lost ray hit in a preview, and neither would raise anything.
    const int escaped = material_outside(blended, pts, hard.bounds);
    CAPTURE(escaped);
    CHECK(escaped > 0);

    // The hard fold adds no extent, so every document that predates layer
    // composition keeps the box it had -- and the smooth one is wider by the
    // combine's SUPPORT, which for a quadratic smin is several times k: the
    // width over which it deviates from the hard min at all, not the depth of
    // the deviation. Wider than the bulge needs, and the same number the item
    // path dilates a smooth item's own bound by.
    CHECK(hard.bounds.max.y == doctest::Approx(0.1f));
    const float ring = layer_blend_support(abutting_slabs(smooth).layers.back());
    CHECK(ring > smooth.blend.k);
    CHECK(blended.bounds.max.y == doctest::Approx(0.1f + ring));
}

TEST_CASE("layer bounds: a hard fold adds no extent, whatever the operator") {
    const Tape add = compile_document(one_item_each(true, composed(Op::Add)));
    for (Op op : {Op::Subtract, Op::Intersect, Op::Paint}) {
        const Tape t = compile_document(one_item_each(true, composed(op)));
        CAPTURE(static_cast<int>(op));
        same_bounds(t, add);
    }
}

TEST_CASE("layer bounds: the ring is the combine's own support, from one expression") {
    // layer_blend_support and group_blend_support are the same function with
    // the fields read from different places; two spellings of "how far this
    // combine reaches" would be one refactor away from disagreeing.
    Document doc = one_item_each(true, composed(Op::Add, BlendProfile::Quadratic, 0.35f));
    Layer& over = doc.layers.back();
    Node group;
    group.is_group = true;
    group.op = over.composition.op;
    group.blend = over.composition.blend;
    group.rounding = over.composition.rounding;
    CHECK(layer_blend_support(over) == doctest::Approx(group_blend_support(group, over)));
    CHECK(layer_blend_support(over) ==
          doctest::Approx(chain_blend_support(over.composition.op, over.composition.blend, 0.0f)));

    SUBCASE("an extended op reaches its own documented support, not its blend") {
        over.composition = composed(Op::Groove, BlendProfile::Hard, 0.5f, 0.12f);
        CHECK(layer_blend_support(over) ==
              doctest::Approx(kernel::ccombine_extended_support(
                  static_cast<int>(Op::Groove), 0.5f, 0.12f)));
    }
    SUBCASE("a layer's rounding converts like a group's, not like an item's") {
        over.xform.scale = 2.0f;
        over.composition = composed(Op::Groove, BlendProfile::Hard, 0.0f, 0.1f);
        CHECK(layer_blend_support(over) == doctest::Approx(0.2f));
    }
}

// -- 3.2: EXACTNESS AND THE LIPSCHITZ BOUND ----------------------------------

TEST_CASE("layer fold: exactness folds as the item combine folds it") {
    // cfi_boolean(x, x) == x, so a HARD fold of two exact fields is still
    // exact; every smooth and every extended fold gives that up.
    CHECK(compile_document(one_item_each(true, composed(Op::Subtract))).info.is_exact);
    CHECK(compile_document(one_item_each(true, composed(Op::Intersect))).info.is_exact);
    CHECK_FALSE(compile_document(one_item_each(true, composed(Op::Add, BlendProfile::Quadratic,
                                                              0.2f)))
                    .info.is_exact);
    CHECK_FALSE(compile_document(several_items(true, composed(Op::Shell, BlendProfile::Hard, 0.1f)))
                    .info.is_exact);
    // A smooth fold with k == 0 is the hard one and costs nothing, exactly as
    // an item's does.
    CHECK(compile_document(one_item_each(true, composed(Op::Add, BlendProfile::Quadratic, 0.0f)))
              .info.is_exact);
}

TEST_CASE("layer fold: a relief composition charges its own slope, not a blend's") {
    // THE REGRESSION. A chain combine used to fold every extended mode through
    // cfi_extended_blend, which against a field info and ITSELF is {false, L}:
    // it charged a relief nothing at all. Relief does not blend two fields --
    // it offsets the accumulated one by an amplitude over a falloff -- so what
    // it costs is that term's gradient, and the item path has always spelled it
    // that way (fold_info's Relief arm). Under the old spelling the safe step
    // came back 3.7x too large and a marcher walked through the relief.
    const float amplitude = 0.3f, width = 0.25f;
    const LayerComposition comp = composed(Op::Relief, BlendProfile::Hard, amplitude, width);

    const kernel::CFieldInfo want = kernel::cfi_relief(kernel::cfi_exact(), amplitude, width);
    CHECK(want.lipschitz > 1.5f);  // teeth: a blend fold would have left it at 1

    const Tape layered = compile_document(one_item_each(true, comp));
    CHECK(layered.info.lipschitz == doctest::Approx(want.lipschitz));
    CHECK_FALSE(layered.info.is_exact);

    SUBCASE("and so does the same relief written as a group") {
        // The group path shares the emitter, so this is where the same defect
        // lived one level down. It had no test at all.
        const Tape grouped = compile_document(several_items(false, comp));
        CHECK(grouped.info.lipschitz == doctest::Approx(want.lipschitz));
    }
    SUBCASE("and the layer form agrees with the item form") {
        // An item's rounding also rounds its own primitive, so the two SHAPES
        // differ here -- but the field info does not, which is the claim.
        Document doc;
        Layer& only = doc.add_sdf_layer("only");
        only.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f, cf3(0.8f, 0.2f, 0.2f)));
        Node relief = sphere_at(cf3(0.7f, 0.1f, 0.0f), 0.6f, cf3(0.2f, 0.3f, 0.9f));
        relief.op = Op::Relief;
        relief.blend.k = amplitude;
        relief.rounding = width;
        only.sdf->insert(relief);
        CHECK(compile_document(doc).info.lipschitz == doctest::Approx(want.lipschitz));
    }
}

TEST_CASE("layer fold: the safe step it reports is a step that does not cross a surface") {
    // The Lipschitz bound is only worth what a marcher can do with it, so this
    // walks the composed field with the step the tape hands out.
    LayerComposition comp;
    SUBCASE("smooth union") { comp = composed(Op::Add, BlendProfile::Quadratic, 0.3f); }
    SUBCASE("smooth subtract") { comp = composed(Op::Subtract, BlendProfile::Quadratic, 0.25f); }
    SUBCASE("intersect") { comp = composed(Op::Intersect); }
    SUBCASE("relief") { comp = composed(Op::Relief, BlendProfile::Hard, 0.2f, 0.3f); }

    const Tape t = compile_document(several_items(true, comp));
    clay_test::check_conservative_steps([&t](cfloat3 p) { return t.eval(p).d; },
                                        t.safe_step_scale(), 2.0f, 600, 11u);
}
