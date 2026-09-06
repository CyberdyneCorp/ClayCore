// THE FOLD BETWEEN LAYERS — visible SDF layers combined by each layer's own
// composition rather than hard-unioned
// (fold-the-layers-with-an-operator, scene-model tasks 2.1-2.4).
//
// The failure this file exists to catch is a wrong FIELD, not an error. Every
// site that folds layers produces a distance for every point whatever operator
// it used, so a fold that picks the wrong one reports nothing at all: the
// geometry is subtly different and no counter, no assertion and no log says so.
// Everything here therefore compares SAMPLES — against an independent reference
// evaluator, against the same shape expressed as items inside one layer, and
// against the document with the layer removed — and every comparison is paired
// with a check that the two sides were not trivially equal already.
//
// WHAT IS NOT HERE, on purpose. The exactness/Lipschitz and bounds halves of
// the parity claim (tasks 3.1-3.3), the four resumable-compile sites and the
// brick refill's own fold (tasks 4.x). This file is the symbolic fold in
// `Compiler::run` and the reference evaluator that has to agree with it.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

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
std::vector<cfloat3> lattice(int n) {
    std::vector<cfloat3> pts;
    pts.reserve(static_cast<std::size_t>(n) * n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                auto a = [n](int v) { return -1.6f + 3.2f * static_cast<float>(v) / (n - 1); };
                pts.push_back(cf3(a(i), a(j), a(k)));
            }
    return pts;
}

// Distance AND colour, so a fold that gets the shape right and the colour wrong
// is still caught — `ctape_combine_values` couples the two for a union and
// carries the accumulator's through for a carve.
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

// How many samples differ, rather than a vector comparison: doctest stringifies
// the operands of a failing CHECK, and a lattice of 4,096 points prints a
// megabyte of floats nobody reads.
int differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++n;
    return n;
}

Node sphere_at(float x, float r, cfloat3 color) {
    Node n;
    n.prim = Prim::sphere(r);
    n.xform.position = cf3(x, 0.0f, 0.0f);
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

// base: a unit sphere at the origin. cutter: a smaller sphere overlapping its
// right-hand side, in its own layer, so the cutter can be composed and hidden
// as a thing.
Document base_and_cutter(LayerComposition comp, bool cutter_visible = true) {
    Document doc;
    Layer& base = doc.add_sdf_layer("base");
    base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
    Layer& cutter = doc.add_sdf_layer("cutter");
    cutter.sdf->insert(sphere_at(0.7f, 0.6f, cf3(0.2f, 0.2f, 0.8f)));
    cutter.composition = comp;
    cutter.visible = cutter_visible;
    return doc;
}

// The reference evaluator, sampled the same way, so the compiler is checked
// against something that never saw a tape.
std::vector<float> reference(const Document& doc, const std::vector<cfloat3>& pts) {
    std::vector<float> out;
    out.reserve(pts.size() * 4);
    for (cfloat3 p : pts) {
        const kernel::CTapeValue v = clay_test::ref_eval_document(doc, p);
        out.push_back(v.d);
        out.push_back(v.color.x);
        out.push_back(v.color.y);
        out.push_back(v.color.z);
    }
    return out;
}

}  // namespace

// -- the fold itself ---------------------------------------------------------

TEST_CASE("a subtractive layer cuts the layer beneath it") {
    const std::vector<cfloat3> pts = lattice(16);
    const Document cut = base_and_cutter(composed(Op::Subtract));
    const Document unioned = base_and_cutter(composed(Op::Add));

    const std::vector<float> cut_s = sample(compile_document(cut), pts);
    const std::vector<float> uni_s = sample(compile_document(unioned), pts);
    // Teeth first: if these were equal the rest of the case would prove nothing.
    CHECK(differing(cut_s, uni_s) > 0);

    // A point well inside the cutter and inside the base is now OUTSIDE.
    const cfloat3 bitten = cf3(0.9f, 0.0f, 0.0f);
    CHECK(compile_document(unioned).eval(bitten).d < 0.0f);
    CHECK(compile_document(cut).eval(bitten).d > 0.0f);
    // ...and a point in the base the cutter does not reach is untouched.
    const cfloat3 kept = cf3(-0.6f, 0.0f, 0.0f);
    CHECK(compile_document(cut).eval(kept).d == compile_document(unioned).eval(kept).d);

    // And the compiler agrees with an evaluator that never saw a tape.
    CHECK(differing(cut_s, reference(cut, pts)) == 0);
    CHECK(differing(uni_s, reference(unioned, pts)) == 0);
}

TEST_CASE("hiding a subtractive layer restores the uncut geometry exactly") {
    const std::vector<cfloat3> pts = lattice(16);
    const Document shown = base_and_cutter(composed(Op::Subtract), true);
    const Document hidden = base_and_cutter(composed(Op::Subtract), false);

    Document base_only;
    Layer& only = base_only.add_sdf_layer("base");
    only.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));

    const std::vector<float> hidden_s = sample(compile_document(hidden), pts);
    // Bit-identical, not merely close: hiding a layer must give back the field
    // the document had before the layer existed, not one that rounds to it.
    CHECK(differing(hidden_s, sample(compile_document(base_only), pts)) == 0);
    CHECK(differing(hidden_s, sample(compile_document(shown), pts)) > 0);  // teeth

    // A hidden layer is not a layer with a different operator, either: the same
    // holds for a smooth subtract, where the fold reaches beyond the cutter.
    const Document smooth = base_and_cutter(composed(Op::Subtract, BlendProfile::Cubic, 0.3f));
    const Document smooth_hidden =
        base_and_cutter(composed(Op::Subtract, BlendProfile::Cubic, 0.3f), false);
    CHECK(differing(sample(compile_document(smooth), pts), hidden_s) > 0);
    CHECK(differing(sample(compile_document(smooth_hidden), pts), hidden_s) == 0);
}

TEST_CASE("the first visible layer initialises and its own operator is not applied") {
    // The whole reason this rule exists: an artist who drags their base layer
    // to the top of the stack, or who sets an operator before there is anything
    // beneath it, must see that layer -- not an empty frame and not an error.
    const std::vector<cfloat3> pts = lattice(14);

    Document plain;
    Layer& p = plain.add_sdf_layer("only");
    p.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
    const std::vector<float> want = sample(compile_document(plain), pts);
    // The layer is really there, so "unchanged" is not "empty either way".
    CHECK(compile_document(plain).eval(cf3(0, 0, 0)).d < 0.0f);

    for (Op op : {Op::Subtract, Op::Intersect, Op::Paint, Op::Shell, Op::Replace}) {
        CAPTURE(static_cast<int>(op));
        Document doc = plain;
        doc.layers[0].composition = composed(op, BlendProfile::Cubic, 0.25f);
        CHECK(differing(sample(compile_document(doc), pts), want) == 0);
        CHECK(differing(sample(compile_document(doc), pts), reference(doc, pts)) == 0);
    }

    SUBCASE("and it is the first VISIBLE one, not the first in the stack") {
        // A hidden layer beneath does not make the one above it "second": with
        // nothing on the accumulator there is still nothing to combine with.
        Document doc = base_and_cutter(composed(Op::Subtract));
        doc.layers[0].visible = false;
        Document cutter_alone;
        Layer& c = cutter_alone.add_sdf_layer("cutter");
        c.sdf->insert(sphere_at(0.7f, 0.6f, cf3(0.2f, 0.2f, 0.8f)));
        CHECK(differing(sample(compile_document(doc), pts),
                        sample(compile_document(cutter_alone), pts)) == 0);
        CHECK(compile_document(doc).eval(cf3(0.7f, 0, 0)).d < 0.0f);  // it is SHOWN
    }
}

TEST_CASE("a layer's operator applies to the layers below it, in stack order") {
    // Order is geometry now. A-B+C and A+C-B are the same three layers.
    const std::vector<cfloat3> pts = lattice(14);
    auto build = [&](bool cutter_in_the_middle) {
        Document doc;
        Layer& a = doc.add_sdf_layer("A");
        a.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
        if (!cutter_in_the_middle) {
            Layer& c = doc.add_sdf_layer("C");
            c.sdf->insert(sphere_at(0.9f, 0.7f, cf3(0.2f, 0.8f, 0.2f)));
        }
        Layer& b = doc.add_sdf_layer("B");
        b.sdf->insert(sphere_at(0.8f, 0.5f, cf3(0.2f, 0.2f, 0.8f)));
        b.composition = composed(Op::Subtract);
        if (cutter_in_the_middle) {
            Layer& c = doc.add_sdf_layer("C");
            c.sdf->insert(sphere_at(0.9f, 0.7f, cf3(0.2f, 0.8f, 0.2f)));
        }
        return doc;
    };
    const Document a_minus_b_plus_c = build(true);
    const Document a_plus_c_minus_b = build(false);
    CHECK(differing(sample(compile_document(a_minus_b_plus_c), pts),
                    sample(compile_document(a_plus_c_minus_b), pts)) > 0);
    CHECK(differing(sample(compile_document(a_minus_b_plus_c), pts),
                    reference(a_minus_b_plus_c, pts)) == 0);
    CHECK(differing(sample(compile_document(a_plus_c_minus_b), pts),
                    reference(a_plus_c_minus_b, pts)) == 0);
}

// -- one emitter, one evaluator ----------------------------------------------

TEST_CASE("two layers and one layer of two items are the same field") {
    // The claim behind "no second vocabulary and no second evaluator": a layer
    // boolean IS the item boolean, so expressing the same shape either way has
    // to give the same field. Distance and colour here; bounds and safe step
    // are the wider parity fixture (task 3.3).
    const std::vector<cfloat3> pts = lattice(16);

    for (Op op : {Op::Add, Op::Subtract, Op::Intersect, Op::Groove, Op::Shell}) {
        for (float k : {0.0f, 0.35f}) {
            CAPTURE(static_cast<int>(op));
            CAPTURE(k);
            const BlendProfile profile = k > 0.0f ? BlendProfile::Cubic : BlendProfile::Hard;

            const Document two = base_and_cutter(composed(op, profile, k));

            // AS AN ITEM. A layer composition carries no rounding here because
            // an item's rounding rounds the item's own PRIMITIVE as well as
            // feeding the combine, and a layer has no primitive to round.
            Document one;
            Layer& l = one.add_sdf_layer("both");
            l.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
            Node cutter = sphere_at(0.7f, 0.6f, cf3(0.2f, 0.2f, 0.8f));
            cutter.op = op;
            cutter.blend.profile = profile;
            cutter.blend.k = k;
            l.sdf->insert(cutter);
            CHECK(differing(sample(compile_document(two), pts),
                            sample(compile_document(one), pts)) == 0);

            // AS A GROUP, which is what a layer is one level up: the same op,
            // blend and ROUNDING, where rounding is the combine's own width and
            // touches no primitive. This is the pairing the layer fold copies.
            const float rounding = 0.04f;
            const Document two_r = base_and_cutter(composed(op, profile, k, rounding));
            Document grouped;
            Layer& g = grouped.add_sdf_layer("both");
            g.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
            Node group;
            group.is_group = true;
            group.op = op;
            group.blend.profile = profile;
            group.blend.k = k;
            group.rounding = rounding;
            const NodeId gid = g.sdf->insert(group);
            g.sdf->insert(sphere_at(0.7f, 0.6f, cf3(0.2f, 0.2f, 0.8f)), gid);
            CHECK(differing(sample(compile_document(two_r), pts),
                            sample(compile_document(grouped), pts)) == 0);
        }
    }

    SUBCASE("and the operator is really being read, so none of that is vacuous") {
        CHECK(differing(sample(compile_document(base_and_cutter(composed(Op::Add))), pts),
                        sample(compile_document(base_and_cutter(composed(Op::Subtract))), pts)) >
              0);
    }
}

// THE FOLD'S ROUNDING FOLLOWS THE LAYER'S SCALE, and nothing else in the suite
// compiles a composed layer at a scale other than 1.
//
// `fold_layer` spells the combine's rb as `comp.rounding *
// layer_distance_scale(layer)` -- a composition has no node, so it takes the
// factor a GROUP's rounding takes rather than the placed one an item's takes.
// That multiplication is what the SIMILARITY verdict rests on: `placement.cpp`
// leaves rounding out of `composition_radius_ignores_scale` precisely because
// the scale reaches it, so a layer carrying only rounding keeps the cheap
// placement path. If the fold stopped scaling it, that verdict would be wrong
// and the only thing asserting it is a CLASSIFICATION, which cannot see a
// field.
//
// Held against `ref_eval_document`, which walks the document without ever
// compiling a tape, at a scale of 1 as the control and at 2 as the claim.
// Only the four modes that READ rb are worth arming: groove and tongue take it
// as the channel half-width, relief and incise as the falloff width, and
// `ctape_combine_dist` ignores the sixth argument for every other mode.
TEST_CASE("a composed layer's rounding scales with the layer") {
    const std::vector<cfloat3> pts = lattice(24);

    for (auto arm : {std::pair{Op::Groove, 0.15f}, std::pair{Op::Tongue, 0.15f},
                     std::pair{Op::Relief, 0.2f}, std::pair{Op::Incise, 0.2f}}) {
        CAPTURE(static_cast<int>(arm.first));
        const LayerComposition comp = composed(arm.first, BlendProfile::Hard, arm.second, 0.1f);

        for (float scale : {1.0f, 2.0f}) {
            CAPTURE(scale);
            Document doc = base_and_cutter(comp);
            doc.layers.back().xform.scale = scale;
            CHECK(differing(sample(compile_document(doc), pts), reference(doc, pts)) == 0);
        }

        // TEETH, in two directions. The scale has to MOVE the field -- otherwise
        // the two arms above are one arm twice...
        Document at_one = base_and_cutter(comp);
        Document at_two = base_and_cutter(comp);
        at_two.layers.back().xform.scale = 2.0f;
        CHECK(differing(sample(compile_document(at_one), pts),
                        sample(compile_document(at_two), pts)) > 0);

        // ...and the ROUNDING has to be part of what moved, or a fold that
        // scaled everything except rb would still pass. Same document, same
        // scale, rounding alone removed.
        LayerComposition no_rb = comp;
        no_rb.rounding = 0.0f;
        Document blunt = base_and_cutter(no_rb);
        blunt.layers.back().xform.scale = 2.0f;
        CHECK(differing(sample(compile_document(at_two), pts),
                        sample(compile_document(blunt), pts)) > 0);
    }
}

// -- symmetry resolves first -------------------------------------------------

TEST_CASE("a layer's symmetry resolves before it folds, once") {
    // A smooth combine does not associate, so folding each mirrored copy into
    // the layers below SEPARATELY is a different field from folding the
    // mirrored layer as one value. The copies are emitted inside the layer's
    // own chain and the fold comes after, which is what this pins: move the
    // layer combine any earlier and the first comparison below breaks.
    const std::vector<cfloat3> pts = lattice(16);
    const float k = 0.35f;

    Document mirrored;
    {
        Layer& base = mirrored.add_sdf_layer("base");
        base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
        Layer& cutter = mirrored.add_sdf_layer("cutter");
        cutter.mirror_axes = kMirrorX;
        Node n = sphere_at(0.8f, 0.5f, cf3(0.2f, 0.2f, 0.8f));
        n.mirror = true;
        cutter.sdf->insert(n);
        cutter.composition = composed(Op::Subtract, BlendProfile::Cubic, k);
    }

    // The same shape written out: both copies in the cutter layer, hard-unioned
    // with each other exactly as the mirror seam unions them, and the LAYER
    // subtracting smoothly once. That is "symmetry first, then the fold".
    Document grouped;
    {
        Layer& base = grouped.add_sdf_layer("base");
        base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
        Layer& cutter = grouped.add_sdf_layer("cutter");
        cutter.sdf->insert(sphere_at(0.8f, 0.5f, cf3(0.2f, 0.2f, 0.8f)));
        cutter.sdf->insert(sphere_at(-0.8f, 0.5f, cf3(0.2f, 0.2f, 0.8f)));
        cutter.composition = composed(Op::Subtract, BlendProfile::Cubic, k);
    }

    // The WRONG shape: each copy subtracted smoothly on its own.
    Document per_copy;
    {
        Layer& base = per_copy.add_sdf_layer("base");
        base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
        for (float x : {0.8f, -0.8f}) {
            Node n = sphere_at(x, 0.5f, cf3(0.2f, 0.2f, 0.8f));
            n.op = Op::Subtract;
            n.blend.profile = BlendProfile::Cubic;
            n.blend.k = k;
            base.sdf->insert(n);
        }
    }

    CHECK(differing(sample(compile_document(mirrored), pts),
                    sample(compile_document(grouped), pts)) == 0);
    // Teeth: the two orders really are different fields, so the first check is
    // an agreement and not a tautology.
    CHECK(differing(sample(compile_document(grouped), pts),
                    sample(compile_document(per_copy), pts)) > 0);
    // ...and the layer's composition is being read at all, so the agreement is
    // an agreement about the FOLD and not about two spellings of a union.
    Document unioned = mirrored;
    unioned.layers[1].composition = LayerComposition{};
    CHECK(differing(sample(compile_document(mirrored), pts),
                    sample(compile_document(unioned), pts)) > 0);
}

// -- a layer that produced nothing -------------------------------------------

TEST_CASE("an intersecting layer with nothing in it empties the document") {
    // max(a, FAR) is FAR. A layer that unions or carves with nothing is
    // identity and is skipped, exactly as every layer was before this feature;
    // one that intersects with nothing is not, and skipping it would leave
    // material the fold removes.
    const std::vector<cfloat3> pts = lattice(12);
    Document doc;
    Layer& base = doc.add_sdf_layer("base");
    base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
    Layer& empty = doc.add_sdf_layer("empty");
    empty.composition = composed(Op::Intersect);
    REQUIRE(empty.sdf->roots.empty());

    const Tape t = compile_document(doc);
    for (cfloat3 p : pts) CHECK(t.eval(p).d > 1e6f);
    CHECK(differing(sample(t, pts), reference(doc, pts)) == 0);

    SUBCASE("while an empty unioning or carving layer changes nothing at all") {
        for (Op op : {Op::Add, Op::Subtract}) {
            CAPTURE(static_cast<int>(op));
            Document d;
            d.add_sdf_layer("base").sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
            d.add_sdf_layer("empty").composition = composed(op);
            Document base_only;
            Layer& only = base_only.add_sdf_layer("base");
            only.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
            // Byte-identical TAPES, not merely equal samples: an empty union
            // must not start emitting an instruction it never emitted, or
            // every document that predates this feature moves.
            const Tape got = compile_document(d);
            const Tape want = compile_document(base_only);
            REQUIRE(got.instrs.size() == want.instrs.size());
            CHECK(std::memcmp(got.params.data(), want.params.data(),
                              want.params.size() * sizeof(float)) == 0);
            CHECK(differing(sample(got, pts), sample(want, pts)) == 0);
        }
    }
}

TEST_CASE("an intersecting layer culled out of a brick still takes the material away") {
    // THE SILENT ONE. Inside a brick the intersecting layer does not reach, its
    // whole chain is culled, and a fold that skips an empty layer would keep
    // material the whole-document tape removes -- in exactly the bricks nobody
    // looks at. The cull's contract is band-clamped identity with the full
    // tape, and that is what this asserts.
    Document doc;
    Layer& base = doc.add_sdf_layer("base");
    base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
    Layer& clip = doc.add_sdf_layer("clip");
    clip.sdf->insert(sphere_at(3.0f, 0.5f, cf3(0.2f, 0.8f, 0.2f)));
    clip.composition = composed(Op::Intersect);

    const Tape full = compile_document(doc);
    const CullRegion cull{math::Aabb{cf3(-0.3f, -0.3f, -0.3f), cf3(0.3f, 0.3f, 0.3f)}};
    const Tape culled = compile_document(doc, &cull);

    const float band = 0.1f;
    int checked = 0;
    clay_test::Lcg rng(451);
    for (int i = 0; i < 400; ++i) {
        const cfloat3 p = rng.vec3(-0.3f, 0.3f);
        ++checked;
        CHECK(kernel::cclamp(full.eval(p).d, -band, band) ==
              kernel::cclamp(culled.eval(p).d, -band, band));
    }
    CHECK(checked == 400);

    SUBCASE("and the base really is inside this region, so the check has teeth") {
        Document without = doc;
        without.layers.pop_back();
        const Tape base_culled = compile_document(without, &cull);
        CHECK(kernel::cclamp(base_culled.eval(cf3(0, 0, 0)).d, -band, band) !=
              kernel::cclamp(culled.eval(cf3(0, 0, 0)).d, -band, band));
    }
}

TEST_CASE("a brick that culls the layers BENEATH still applies the layer above") {
    // THE MIRROR OF THE ONE ABOVE, and the one that was missing. There the
    // COMPOSED layer was culled away; here everything BENEATH it is, which is
    // the ordinary case for a cutter that sits away from the base -- and it is
    // the sharper of the two, because "which layer is first" is a property of
    // the DOCUMENT and a per-compile accumulator flag is not.
    //
    // Deciding first-ness from that flag promotes the composed layer to the
    // initialiser FOR THAT BRICK ONLY: its operator is silently not applied and
    // the brick returns a field the document does not have -- an intersecting
    // cutter renders as a solid sphere, a subtracting one as a lump. No error,
    // no counter, one brick different from its neighbour.
    for (Op op : {Op::Intersect, Op::Subtract}) {
        CAPTURE(static_cast<int>(op));
        Document doc;
        Layer& base = doc.add_sdf_layer("base");
        base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
        Layer& clip = doc.add_sdf_layer("clip");
        clip.sdf->insert(sphere_at(3.0f, 0.5f, cf3(0.2f, 0.8f, 0.2f)));
        clip.composition = composed(op);

        // A brick over the clip layer's own sphere, far enough from the base
        // that the base is culled out of it entirely.
        const CullRegion cull{math::Aabb{cf3(2.7f, -0.3f, -0.3f), cf3(3.3f, 0.3f, 0.3f)}};
        const Tape full = compile_document(doc);
        const Tape culled = compile_document(doc, &cull);

        // Neither disjoint sphere survives an intersect and a subtract cannot
        // create material, so the whole document is empty here -- and the
        // per-brick tape has to say the same thing.
        int solid_full = 0, solid_culled = 0;
        clay_test::Lcg rng(9182);
        for (int i = 0; i < 512; ++i) {
            const cfloat3 p = cf3(3.0f, 0.0f, 0.0f) + rng.vec3(-0.3f, 0.3f);
            if (full.eval(p).d < 0.0f) ++solid_full;
            if (culled.eval(p).d < 0.0f) ++solid_culled;
        }
        CHECK(solid_full == 0);
        CHECK(solid_culled == 0);

        SUBCASE("and the clip layer's own sphere really is solid there on its own") {
            // Teeth: without the fold this region IS material, so the two
            // counts above are zero because the operator was applied and not
            // because the fixture put nothing there.
            Document alone;
            alone.add_sdf_layer("clip").sdf->insert(sphere_at(3.0f, 0.5f, cf3(0, 1, 0)));
            const Tape t = compile_document(alone, &cull);
            CHECK(t.eval(cf3(3.0f, 0.0f, 0.0f)).d < 0.0f);
        }
    }
}

TEST_CASE("the band a culled brick reports matches the whole document, three layers deep") {
    // The variant that keeps the refill SPLIT enabled: a plain unioning layer
    // on top means `layer_join_is_hard_union` is true, so the two halves are
    // compiled and stored -- and a `below` half that promoted the intersecting
    // middle layer to initialiser would be stored as a seed and answered from
    // for as long as the revision stands.
    Document doc;
    Layer& base = doc.add_sdf_layer("base");
    base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
    Layer& clip = doc.add_sdf_layer("clip");
    clip.sdf->insert(sphere_at(2.6f, 0.7f, cf3(0.2f, 0.8f, 0.2f)));
    clip.composition = composed(Op::Intersect);
    Layer& top = doc.add_sdf_layer("top");
    top.sdf->insert(sphere_at(3.4f, 0.4f, cf3(0.2f, 0.2f, 0.8f)));

    const Tape full = compile_document(doc);
    const CullRegion cull{math::Aabb{cf3(2.9f, -0.4f, -0.4f), cf3(3.7f, 0.4f, 0.4f)}};
    const Tape culled = compile_document(doc, &cull);

    // Band-clamped identity inside the region, which is the cull's contract.
    const float band = 0.1f;
    int differing_band = 0;
    clay_test::Lcg rng(5150);
    for (int i = 0; i < 800; ++i) {
        const cfloat3 p = cf3(3.3f, 0.0f, 0.0f) + rng.vec3(-0.4f, 0.4f);
        if (kernel::cclamp(full.eval(p).d, -band, band) !=
            kernel::cclamp(culled.eval(p).d, -band, band))
            ++differing_band;
    }
    CHECK(differing_band == 0);
    CHECK(layer_join_is_hard_union(doc));  // the split really is available here
}

TEST_CASE("an empty layer beneath is still the layer that opens the stack") {
    // The uncull'd statement of the same rule, and the one that pins WHICH
    // definition of "first" this compiler uses: the first visible SDF LAYER,
    // whether or not its chain produced anything. An empty base layer under an
    // intersecting one leaves the document empty -- exactly as an item chain
    // that opens with an intersecting item does, which is what keeps the two
    // forms of one shape one shape.
    //
    // It has to be a rule the CULL cannot reach, and "the layer produced a
    // value" is not one: a brick decides that for itself. This is the price of
    // that, stated where it can be seen rather than discovered.
    const std::vector<cfloat3> pts = lattice(12);
    Document doc;
    Layer& base = doc.add_sdf_layer("base");  // visible, SDF, and empty
    REQUIRE(base.sdf->roots.empty());
    Layer& clip = doc.add_sdf_layer("clip");
    clip.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.2f, 0.8f, 0.2f)));
    clip.composition = composed(Op::Intersect);

    const Tape t = compile_document(doc);
    for (cfloat3 p : pts) CHECK(t.eval(p).d > 1e6f);
    // The independent evaluator has to have reached the same rule from the
    // same document, or a fold bug is invisible wherever the fixtures union.
    CHECK(differing(sample(t, pts), reference(doc, pts)) == 0);

    SUBCASE("and the same layer unions onto it as itself") {
        Document d;
        d.add_sdf_layer("base");
        d.add_sdf_layer("clip").sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.2f, 0.8f, 0.2f)));
        const Tape got = compile_document(d);
        CHECK(got.eval(cf3(0, 0, 0)).d == doctest::Approx(-1.0f));
        CHECK(differing(sample(got, pts), reference(d, pts)) == 0);
    }
}
