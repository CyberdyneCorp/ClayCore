// THE SITES THAT ASSUMED A HARD UNION BETWEEN LAYERS
// (fold-the-layers-with-an-operator, tasks 4.1-4.6 and 5.1-5.3).
//
// Visible SDF layers fold under each layer's own composition now, and the fold
// itself is test_layer_fold.cpp's. This file is about everywhere ELSE the
// document's inter-layer combine was assumed to be a hard Add: the resumable
// prefix reuse, the two-part split a brick refill takes, the Except/Only
// pairing a host previews one layer with, the brick refill's own host-side
// rejoin, the cull pad, and the dirty region a composed layer's own commands
// have to name.
//
// WHY EVERY CASE HERE COMPARES A FIELD. A site that folds with the wrong
// operator does not fail: it returns a distance for every point, so there is no
// error, no assertion and no visual tell beyond geometry that is subtly wrong.
// The only way to hold these is to run the fast path and the slow path over the
// same document and compare SAMPLES, and to pair every comparison with a check
// that the two sides were not already equal.
//
// AND WHY THE REFUSALS ARE ASSERTED AS COUNTS. Where the policy is to refuse a
// fast path rather than teach it the operator, the refusal is invisible in the
// values by construction -- a refused resume answers exactly what a full walk
// answers. `clay_document_resume_stats` is what separates them, so the claim is
// made against `resumed_bricks`, which is a count, and never against a clock.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"
#include "clay/kernel/tape.h"
#include "clay_internal.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/document.h"
#include "clay/scene/placement.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::scene;
using kernel::cf3;
using kernel::cfloat3;

namespace {

LayerComposition composed(Op op, BlendProfile profile = BlendProfile::Hard, float k = 0.0f,
                          float rounding = 0.0f) {
    LayerComposition c;
    c.op = op;
    c.blend.profile = profile;
    c.blend.k = k;
    c.rounding = rounding;
    return c;
}

Node sphere_at(float x, float r) {
    Node n;
    n.prim = Prim::sphere(r);
    n.xform.position = cf3(x, 0.0f, 0.0f);
    n.color = cf3(0.3f, 0.6f, 0.9f);
    return n;
}

// Three visible SDF layers, low to high, so "the seam" (the last one) and "any
// fold" (the middle one) are different questions and a predicate that answers
// one for the other is caught.
Document three_layers() {
    Document doc;
    Layer& a = doc.add_sdf_layer("a");
    a.sdf->insert(sphere_at(0.0f, 1.0f));
    Layer& b = doc.add_sdf_layer("b");
    b.sdf->insert(sphere_at(0.8f, 0.6f));
    Layer& c = doc.add_sdf_layer("c");
    c.sdf->insert(sphere_at(-0.8f, 0.6f));
    return doc;
}

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

// Distance and colour, because the combine couples them.
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

// A count rather than a vector comparison: doctest stringifies both operands of
// a failing CHECK, and a lattice of 4,096 points prints a megabyte of floats.
int differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++n;
    return n;
}

// The two halves of a split, rejoined the way a caller holding two VALUES
// rejoins them: sample by sample, through the kernel's own combine, exactly as
// the C ABI's `fold_layers_below` does.
std::vector<float> rejoin(const Tape& below, const Tape& only, const std::vector<cfloat3>& pts,
                          const LayerComposition& comp, float round_world) {
    std::vector<float> out;
    out.reserve(pts.size() * 4);
    for (cfloat3 p : pts) {
        const kernel::CTapeValue r = kernel::ctape_combine_values(
            below.eval(p), only.eval(p), static_cast<int>(comp.op),
            static_cast<int>(comp.blend.profile), comp.blend.k, round_world);
        out.push_back(r.d);
        out.push_back(r.color.x);
        out.push_back(r.color.y);
        out.push_back(r.color.z);
    }
    return out;
}

}  // namespace

// -- the predicates the other sites detect with -------------------------------

TEST_CASE("the join predicates: the seam is one question, every fold another") {
    Document doc = three_layers();
    const LayerId a = doc.layers[0].id, b = doc.layers[1].id, c = doc.layers[2].id;

    SUBCASE("a document that unions throughout answers yes to both") {
        CHECK(layer_join_is_hard_union(doc));
        CHECK(first_composed_fold_layer(doc) == 0);
        // The join at the seam is the LAST layer's composition, and it is
        // applied, so there is one to report.
        const LayerComposition* join = layer_join_composition(doc, c);
        REQUIRE(join != nullptr);
        CHECK(join->op == Op::Add);
    }

    SUBCASE("a composed layer in the MIDDLE keeps the seam and loses the sum") {
        doc.find_layer(b)->composition = composed(Op::Subtract);
        CHECK(layer_join_is_hard_union(doc));  // the split is taken at `c`, which unions
        CHECK(first_composed_fold_layer(doc) == b);
    }

    SUBCASE("a composed layer at the TOP loses both") {
        doc.find_layer(c)->composition = composed(Op::Subtract);
        CHECK_FALSE(layer_join_is_hard_union(doc));
        CHECK(first_composed_fold_layer(doc) == c);
    }

    SUBCASE("the FIRST visible layer's composition is not applied, so it breaks nothing") {
        doc.find_layer(a)->composition = composed(Op::Intersect, BlendProfile::Cubic, 0.4f);
        CHECK(layer_join_is_hard_union(doc));
        CHECK(first_composed_fold_layer(doc) == 0);
    }

    SUBCASE("a hidden layer is not in the fold, above or below") {
        doc.find_layer(a)->composition = composed(Op::Subtract);
        doc.find_layer(a)->visible = false;
        doc.find_layer(b)->composition = composed(Op::Subtract);
        // `b` is now the first VISIBLE layer, so its operator is not applied.
        CHECK(first_composed_fold_layer(doc) == 0);
        doc.find_layer(c)->visible = false;
        // ...and with `c` gone the seam is `b`, which is still the first.
        CHECK(layer_join_is_hard_union(doc));
    }

    SUBCASE("one visible SDF layer has no join at all, however it is composed") {
        Document one;
        Layer& only = one.add_sdf_layer("only");
        only.sdf->insert(sphere_at(0.0f, 1.0f));
        only.composition = composed(Op::Intersect, BlendProfile::Quadratic, 0.3f, 0.1f);
        CHECK(layer_join_is_hard_union(one));
        CHECK(first_composed_fold_layer(one) == 0);
        CHECK(layer_join_composition(one, only.id) == nullptr);
    }

    SUBCASE("a non-SDF or absent layer has no join") {
        CHECK(layer_join_composition(doc, 9999) == nullptr);
        CHECK(layer_join_composition(doc, a) == nullptr);  // nothing beneath it
    }
}

// -- 4.2 compile_document_part: the halves rejoin under the ACTIVE composition -

TEST_CASE("part split: the two halves rejoin under the active layer's own composition") {
    const std::vector<cfloat3> pts = lattice(16);

    for (LayerComposition comp :
         {composed(Op::Add), composed(Op::Subtract), composed(Op::Intersect),
          composed(Op::Add, BlendProfile::Cubic, 0.3f), composed(Op::Groove, BlendProfile::Hard,
                                                                 0.0f, 0.15f)}) {
        CAPTURE(static_cast<int>(comp.op));
        Document doc = three_layers();
        const LayerId top = doc.layers.back().id;
        doc.layers.back().composition = comp;

        const Tape whole = compile_document(doc);
        const Tape below = compile_document_part(doc, top, /*below=*/true);
        const Tape only = compile_document_part(doc, top, /*below=*/false);

        const LayerComposition* join = layer_join_composition(doc, top);
        REQUIRE(join != nullptr);
        const float round_world = join->rounding * layer_distance_scale(*doc.find_layer(top));
        const std::vector<float> rejoined = rejoin(below, only, pts, *join, round_world);
        // BIT-IDENTICAL. The split is taken at a layer boundary, so `below` is
        // exactly the accumulator the whole-document walk holds when it reaches
        // the active layer -- not a partial value that a later fold would have
        // treated differently.
        CHECK(differing(rejoined, sample(whole, pts)) == 0);
    }

    SUBCASE("and rejoining with a hard Add instead is a different field") {
        // The teeth for every arm above: if the operator did not matter here,
        // none of those comparisons would have been testing anything.
        Document doc = three_layers();
        const LayerId top = doc.layers.back().id;
        doc.layers.back().composition = composed(Op::Subtract);
        const Tape below = compile_document_part(doc, top, true);
        const Tape only = compile_document_part(doc, top, false);
        const std::vector<float> wrong = rejoin(below, only, pts, composed(Op::Add), 0.0f);
        CHECK(differing(wrong, sample(compile_document(doc), pts)) > 0);
    }

    SUBCASE("the halves are still each a correct compile of their own layers") {
        // The engine refuses nothing here: only the JOIN changes.
        Document doc = three_layers();
        const LayerId top = doc.layers.back().id;
        doc.layers.back().composition = composed(Op::Intersect);
        Document without_top = doc;
        without_top.layers.pop_back();
        CHECK(differing(sample(compile_document_part(doc, top, true), pts),
                        sample(compile_document(without_top), pts)) == 0);
    }
}

// -- 4.3 compile_document_except: the sum promise is gone ---------------------

TEST_CASE("except/only no longer compose back to the whole once a layer folds") {
    const std::vector<cfloat3> pts = lattice(14);
    Document doc = three_layers();
    const LayerId mid = doc.layers[1].id;

    auto min_composed = [&](const Document& d) {
        const Tape except = compile_document_except(d, mid);
        const Tape only = compile_document_part(d, mid, /*below=*/false);
        return rejoin(except, only, pts, composed(Op::Add), 0.0f);
    };

    // While every layer unions, min() IS the document -- which is the promise
    // the C ABI made and now polices rather than makes.
    CHECK(differing(min_composed(doc), sample(compile_document(doc), pts)) == 0);

    // With the middle layer subtracting there is no composition of the two
    // parts that reconstructs it: `except` holds a+c, `only` holds b, and the
    // document is (a-b)+c.
    doc.find_layer(mid)->composition = composed(Op::Subtract);
    CHECK(differing(min_composed(doc), sample(compile_document(doc), pts)) > 0);

    SUBCASE("but the compile itself is still the document without that layer") {
        Document removed = doc;
        removed.layers.erase(removed.layers.begin() + 1);
        CHECK(differing(sample(compile_document_except(doc, mid), pts),
                        sample(compile_document(removed), pts)) == 0);
    }
}

// -- 4.4 compile_document_append refuses a composed seam ----------------------

TEST_CASE("prefix reuse refuses a composed seam, and the full compile is the answer") {
    auto try_append = [](LayerComposition comp, Tape* out) {
        Document doc = three_layers();
        doc.layers.back().composition = comp;
        TapeCheckpoint cp;
        const Tape prefix = compile_document_resumable(doc, &cp);
        // One more item at the tail of the active layer's root list.
        const NodeId added = doc.layers.back().sdf->insert(sphere_at(-1.1f, 0.4f));
        const std::vector<NodeId> appended{added};
        Tape reused;
        TapeCheckpoint out_cp;
        const bool ok =
            compile_document_append(prefix, cp, doc, appended, &reused, &out_cp);
        *out = compile_document(doc);
        return ok;
    };

    SUBCASE("a hard union at the seam still reuses") {
        Tape full;
        CHECK(try_append(composed(Op::Add), &full));
    }

    // Every one of these makes the carry-over of `info`,
    // `lipschitz_bounds_gradient` and `bounds` false, so the reuse is refused
    // rather than made almost-right. MEASURED with the refusal removed, on the
    // smooth-union arm below: the reused tape reports bounds.min.x = -1.5 where
    // the whole-document compile reports -3.0, because the fold's ring is
    // applied to the layer's extent at compile time and the appended item
    // enlarges it. A box that small is a lost ray hit and a dropped brick --
    // missing surface, not an error -- which is why this is a refusal.
    for (LayerComposition comp :
         {composed(Op::Subtract), composed(Op::Intersect),
          composed(Op::Add, BlendProfile::Cubic, 0.25f), composed(Op::Add, BlendProfile::Hard,
                                                                  0.0f, 0.2f)}) {
        CAPTURE(static_cast<int>(comp.op));
        CAPTURE(comp.blend.k);
        CAPTURE(comp.rounding);
        Tape full;
        CHECK_FALSE(try_append(comp, &full));
        // ...and a refused caller compiles in full, which is a real tape.
        CHECK(full.instrs.size() > 0);
    }

    SUBCASE("a composed layer BENEATH the seam does not cost the reuse") {
        // The seam is the last visible SDF layer and nothing else; a cutter
        // underneath the layer being sculpted keeps the fast path.
        Document doc = three_layers();
        doc.layers[1].composition = composed(Op::Subtract, BlendProfile::Cubic, 0.2f);
        TapeCheckpoint cp;
        const Tape prefix = compile_document_resumable(doc, &cp);
        const NodeId added = doc.layers.back().sdf->insert(sphere_at(-1.1f, 0.4f));
        Tape reused;
        TapeCheckpoint out_cp;
        REQUIRE(compile_document_append(prefix, cp, doc, {added}, &reused, &out_cp));
        // And it is the whole-document compile, byte for byte.
        const Tape full = compile_document(doc);
        REQUIRE(reused.instrs.size() == full.instrs.size());
        CHECK(std::memcmp(reused.instrs.data(), full.instrs.data(),
                          full.instrs.size() * sizeof(kernel::CTapeInstr)) == 0);
        REQUIRE(reused.params.size() == full.params.size());
        CHECK(std::memcmp(reused.params.data(), full.params.data(),
                          full.params.size() * sizeof(float)) == 0);
    }
}

// -- 5.1 the cull pad has a term for the fold's own blend ---------------------

TEST_CASE("a smooth layer fold pads the document's cull") {
    // The failure this catches: a per-brick compile drops an item the
    // whole-document compile keeps, because the item is outside the region and
    // the pad has no term for the fold that drags the value inside it. Nothing
    // errors; the samples inside the band just differ.
    Document doc;
    Layer& low = doc.add_sdf_layer("low");
    low.sdf->insert(sphere_at(0.0f, 0.5f));   // box [-0.5, 0.5]
    Layer& high = doc.add_sdf_layer("high");
    high.sdf->insert(sphere_at(0.9f, 0.5f));  // box [0.4, 1.4]
    high.composition = composed(Op::Add, BlendProfile::Quadratic, 0.45f);

    // A region beyond the lower layer's own box, and near enough that the
    // smooth fold still drags the value there.
    CullRegion cull;
    cull.region = math::Aabb{cf3(0.56f, -0.05f, -0.05f), cf3(0.68f, 0.05f, 0.05f)};

    const Tape whole = compile_document(doc);
    const Tape culled = compile_document(doc, &cull);

    std::vector<cfloat3> pts;
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 3; ++j)
            pts.push_back(cf3(0.56f + 0.02f * static_cast<float>(i),
                              -0.04f + 0.04f * static_cast<float>(j), 0.0f));

    CHECK(differing(sample(culled, pts), sample(whole, pts)) == 0);

    SUBCASE("through the cached index too, which is the pad a refill actually uses") {
        // document_pad has two arms -- the per-compile walk above and
        // CullIndex::cull_pad() -- and a term in one of them only would cull a
        // brick refill differently from a whole-document compile.
        const CullIndex index(doc);
        CHECK(index.cull_pad() > 0.0f);
        CHECK(differing(sample(compile_document(doc, &cull, &index), pts),
                        sample(whole, pts)) == 0);
    }

    SUBCASE("and the lower layer really does decide those samples") {
        // The teeth. Without it the case above would pass on a document whose
        // culled tape had nothing to lose.
        Document alone = doc;
        alone.layers.erase(alone.layers.begin());
        CHECK(differing(sample(compile_document(alone), pts), sample(whole, pts)) > 0);
    }

    SUBCASE("a hard fold still pads nothing, so nothing that unions pays") {
        // Asserted at the DOCUMENT level, which is where the fold's term lives:
        // `cull_pad` is one layer's own chain and has never carried one, since
        // a fold drags the layers BENEATH it rather than the one that owns it
        // (bounds.cpp, cull_pad_terms).
        Document hard = doc;
        hard.layers.back().composition = LayerComposition{};
        CHECK(document_cull_pad(hard) == 0.0f);
        CHECK(document_cull_pad(doc) > 0.0f);
    }
}

// -- 5.1 and the pad SUMS the folds above a layer -----------------------------

namespace {

// Four layers, one hard sphere each, 0.62 apart so no two layers' own boxes
// overlap: every disagreement the sweep below finds arrived through a FOLD and
// not through geometry a region already contained.
//
// `composed_folds` counts from the TOP, which is the shape an artist makes -- a
// base, with joins and cutters stacked above it. The bottom layer's own
// composition is never applied (tape.h), so four layers carry three folds.
Document fold_stack(int composed_folds, float k) {
    Document doc;
    for (int i = 0; i < 4; ++i) {
        Layer& l = doc.add_sdf_layer("l" + std::to_string(i));
        l.sdf->insert(sphere_at(0.62f * static_cast<float>(i), 0.5f));
    }
    for (int i = 0; i < composed_folds; ++i)
        doc.layers[static_cast<std::size_t>(3 - i)].composition =
            composed(Op::Add, BlendProfile::Quadratic, k);
    return doc;
}

// The worst band-clamped disagreement between a REGION-LIMITED compile and the
// whole-document one, swept over 240 small regions crossing the whole stack.
//
// This is the shape a brick refill has, and it is the only shape that finds
// this class of bug: one narrow region at a time, each dilated by its own band
// exactly as a brick's CullRegion is, sampled INSIDE the band, which is where
// the culled tape's promise applies. One wide region keeps every item and
// agrees with everything.
//
// `extra` is diagnostic rather than asserted: widening the region by hand is
// what separates "the pad is too narrow" from "the fold is wrong", since a
// wrong fold does not improve when the compile is handed more to look at.
float sweep_worst(const Document& doc, float band, float extra = 0.0f) {
    const CullIndex index(doc);
    const Tape whole = compile_document(doc);
    float worst = 0.0f;
    for (int i = 0; i < 240; ++i) {
        const float x0 = -1.2f + 3.8f * static_cast<float>(i) / 240.0f;
        const math::Aabb r{cf3(x0, -0.03f, -0.03f), cf3(x0 + 0.06f, 0.03f, 0.03f)};
        CullRegion cull;
        cull.region = r.dilated(band + extra);
        const Tape culled = compile_document(doc, &cull, &index);
        for (int s = 0; s < 7; ++s) {
            const cfloat3 p = cf3(x0 + 0.06f * static_cast<float>(s) / 6.0f, 0.0f, 0.0f);
            const float a = whole.eval(p).d;
            const float b = culled.eval(p).d;
            if (std::fabs(a) <= band || std::fabs(b) <= band)
                worst = kernel::cmax(worst, std::fabs(a - b));
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("the cull pad sums the folds above a layer rather than taking the largest") {
    // THE FOURTH CULL-OBSERVABLE PREDICATE (design.md 13). `cull_pad_terms`
    // answers what ONE layer's item chain needs, and both readers of the
    // document's pad are a MAXIMUM over layers of it -- so a document of N
    // composed folds was padded for one of them, while the drag an item at the
    // bottom of the stack passes through is the SUM of every fold above it.
    //
    // Measured on this fixture before the terms were summed: worst band drift 0
    // at one composed fold -- which is all the case above ever exercised --
    // 0.0180 at two with k = 0.3, and 0.0229 / 0.0268 at three with k = 0.3 /
    // 0.45. The all-hard row was 0, so the sweep invents nothing of its own.
    // Dilating every region by a further 2k took the two-fold row to 0 and the
    // three-fold row to 0.0049: the shortfall scales with the fold COUNT, which
    // is what identified the pad rather than the fold.
    const float band = 0.1f;

    SUBCASE("a stack that unions hard is identical, so the sweep has no false positives") {
        for (float k : {0.15f, 0.3f, 0.45f}) CHECK(sweep_worst(fold_stack(0, k), band) == 0.0f);
    }

    SUBCASE("and so is one, two or three composed folds") {
        for (int folds = 1; folds <= 3; ++folds)
            for (float k : {0.15f, 0.3f, 0.45f})
                CHECK(sweep_worst(fold_stack(folds, k), band) == 0.0f);
    }

    SUBCASE("because the pad grows with the NUMBER of folds") {
        // The teeth, and the one assertion that separates a sum from a maximum:
        // under a maximum these three numbers are equal. A value, not a clock.
        const float one = document_cull_pad(fold_stack(1, 0.3f));
        const float two = document_cull_pad(fold_stack(2, 0.3f));
        const float three = document_cull_pad(fold_stack(3, 0.3f));
        CHECK(document_cull_pad(fold_stack(0, 0.3f)) == 0.0f);
        CHECK(one > 0.0f);
        CHECK(two == doctest::Approx(2.0f * one));  // exactly one term per fold
        CHECK(three == doctest::Approx(3.0f * one));
    }

    SUBCASE("and it is charged to the layers BENEATH each fold, which are the ones that need it") {
        // Attribution. A fold drags the items underneath it, so the term rises
        // going DOWN the stack. While it rode the layer that OWNED the fold
        // only the document-wide maximum hid the misattribution, and fixing the
        // sum without fixing the attribution would have moved the error rather
        // than closed it.
        const Document doc = fold_stack(3, 0.3f);
        const float bottom = folds_from_layer_support(doc, doc.layers.front().id);
        const float top = folds_from_layer_support(doc, doc.layers.back().id);
        CHECK(top > 0.0f);
        CHECK(bottom == doctest::Approx(3.0f * top));
    }

    SUBCASE("and the first visible layer's own composition is not a term") {
        // It is never applied (tape.h), so counting it pads the whole document
        // for a fold that does not exist. Over-wide is the cheap direction and
        // still not free -- it keeps items a compile did not need and costs
        // tape -- and the exact answer is in hand here.
        Document doc = fold_stack(0, 0.0f);
        doc.layers.front().composition = composed(Op::Subtract, BlendProfile::Quadratic, 0.4f);
        CHECK(folds_from_layer_support(doc, doc.layers.front().id) == 0.0f);
        CHECK(document_cull_pad(doc) == 0.0f);
        // And it IS a term for every layer that is not the first one.
        doc.layers[1].composition = composed(Op::Subtract, BlendProfile::Quadratic, 0.4f);
        CHECK(folds_from_layer_support(doc, doc.layers.front().id) > 0.0f);
        CHECK(folds_from_layer_support(doc, doc.layers[1].id) ==
              folds_from_layer_support(doc, doc.layers.front().id));
    }

    SUBCASE("and the cached index reports exactly the same number") {
        // The pad's two readers, held equal by a test rather than by a comment:
        // a compile takes whichever it has (Compiler::document_pad), so a term
        // in one of them alone would cull a brick refill differently from the
        // whole-document compile it is supposed to agree with.
        for (int folds = 0; folds <= 3; ++folds) {
            const Document doc = fold_stack(folds, 0.3f);
            CHECK(CullIndex(doc).cull_pad() == document_cull_pad(doc));  // exact, not approx
        }
    }
}


// -- 5.3 the dirty region a composed layer's own commands name ----------------

TEST_CASE("an intersecting layer's commands dirty what it can take away") {
    Document doc;
    Layer& low = doc.add_sdf_layer("low");
    low.sdf->insert(sphere_at(-2.0f, 0.9f));  // far to the LEFT of everything above
    Layer& high = doc.add_sdf_layer("high");
    high.sdf->insert(sphere_at(1.5f, 0.4f));
    const LayerId high_id = high.id;

    auto bound_of = [&](const Document& d, const Command& cmd) {
        return command_influence_bound(d, cmd);
    };

    const math::Aabb own = layer_influence_bound(*doc.find_layer(high_id));

    SUBCASE("a subtracting layer is bounded by itself") {
        doc.find_layer(high_id)->composition = composed(Op::Subtract);
        const math::Aabb b = bound_of(doc, Command{SetLayerVisibleCmd{high_id, false}});
        // max(a, -b) far from b's geometry is a: a subtract cannot take
        // material away where it has none.
        CHECK(b.min.x == doctest::Approx(own.min.x));
        CHECK(b.max.x == doctest::Approx(own.max.x));
    }

    SUBCASE("an intersecting layer reaches the layers beneath it") {
        doc.find_layer(high_id)->composition = composed(Op::Intersect);
        const math::Aabb b = bound_of(doc, Command{SetLayerVisibleCmd{high_id, false}});
        const math::Aabb below = layer_influence_bound(doc.layers.front());
        // Hiding it gives back everything it was removing, which is everywhere
        // the accumulator had material.
        CHECK(b.min.x <= below.min.x);
        CHECK(b.max.x >= own.max.x);
        CHECK(b.min.x < own.min.x);  // strictly wider than the layer's own box
    }

    SUBCASE("a composition command takes the same widening") {
        doc.find_layer(high_id)->composition = composed(Op::Intersect);
        const math::Aabb b =
            bound_of(doc, Command{SetLayerCompositionCmd{high_id, composed(Op::Add)}});
        CHECK(b.min.x <= layer_influence_bound(doc.layers.front()).min.x);
    }

    SUBCASE("a smooth fold dilates by its own support") {
        doc.find_layer(high_id)->composition = composed(Op::Add, BlendProfile::Quadratic, 0.5f);
        const math::Aabb b = bound_of(doc, Command{SetLayerVisibleCmd{high_id, false}});
        CHECK(b.min.x < own.min.x);
        CHECK(b.max.x > own.max.x);
        // ...and it is the FOLD's support, the one expression the item path uses.
        const float support = layer_blend_support(*doc.find_layer(high_id));
        CHECK(support > 0.0f);
        CHECK(b.max.x == doctest::Approx(own.max.x + support));
    }

    SUBCASE("a plain unioning layer is unchanged, which every old document is") {
        const math::Aabb b = bound_of(doc, Command{SetLayerVisibleCmd{high_id, false}});
        CHECK(b.min.x == doctest::Approx(own.min.x));
        CHECK(b.max.x == doctest::Approx(own.max.x));
    }
}

TEST_CASE("a layer whose composition blends softly does not scale cleanly") {
    // The placement gesture skips work on the strength of this verdict, so a
    // wrong Similarity is a preview that lags its own field. The layer's ITEMS
    // all scale cleanly here; only the composition's radius does not follow.
    Document doc;
    Layer& l = doc.add_sdf_layer("cutter");
    l.sdf->insert(sphere_at(0.0f, 1.0f));  // hard, no blend radius
    CHECK(layer_scales_cleanly(l));

    l.composition = composed(Op::Subtract, BlendProfile::Cubic, 0.2f);
    CHECK_FALSE(layer_scales_cleanly(l));

    SUBCASE("rounding DOES follow the scale, so it is not part of the test") {
        l.composition = composed(Op::Subtract, BlendProfile::Hard, 0.0f, 0.2f);
        CHECK(layer_scales_cleanly(l));
    }

    SUBCASE("and a hard fold with a zero radius has nothing to be wrong about") {
        l.composition = composed(Op::Intersect);
        CHECK(layer_scales_cleanly(l));
    }

    SUBCASE("the placement change it feeds reports GENERAL") {
        l.composition = composed(Op::Subtract, BlendProfile::Cubic, 0.2f);
        math::Transform to = l.xform;
        to.scale = 2.0f;
        CHECK(layer_placement_change(l, to, cf3(1.0f, 1.0f, 1.0f)).kind == PlacementKind::General);
        l.composition = LayerComposition{};
        CHECK(layer_placement_change(l, to, cf3(1.0f, 1.0f, 1.0f)).kind ==
              PlacementKind::Similarity);
    }

    SUBCASE("A RADIUS IS A RADIUS: every op the setter takes, hard profile") {
        // EVERY op, rather than the extended range plus a hand-kept list. The
        // first version of this predicate read the profile and then rescued
        // `op_is_extended`, which left PAINT -- numerically BELOW the extended
        // range -- falling through both clauses: a hard-profile paint with
        // k = 0.3 fades its colour over an absolute 0.3 (ctape_combine_values
        // takes `cmax(blend_support(profile, k), k)`) and classified as a
        // SIMILARITY. Sweeping the whole vocabulary is what makes that a test
        // rather than a second list to keep in step.
        //
        // The three plain booleans are here too, and they are the CONSERVATIVE
        // direction rather than an accident: a hard Add, Subtract or Intersect
        // ignores k in the field, and reports GENERAL anyway because
        // `layer_blend_support` -- and through it the DOCUMENT'S cull pad --
        // reads that same k as 0.3 whatever the op. One field, one reading.
        for (Op op : {Op::Add, Op::Subtract, Op::Intersect, Op::Paint, Op::Groove, Op::Tongue,
                      Op::Pipe, Op::Engrave, Op::Emboss, Op::Inset, Op::Shell, Op::Replace,
                      Op::Relief, Op::Incise}) {
            CAPTURE(static_cast<int>(op));
            l.composition = composed(op, BlendProfile::Hard, 0.15f);
            CHECK_FALSE(layer_scales_cleanly(l));
            // ...and with no radius at all there is still nothing to be wrong
            // about, so this did not simply refuse every op.
            l.composition = composed(op, BlendProfile::Hard, 0.0f);
            CHECK(layer_scales_cleanly(l));
        }
    }
}

// -- 4.5 the brick refill's multi-layer split, across the C ABI ---------------

namespace {

// Two SDF layers through the ABI, the upper one being the one a stroke appends
// to and the one whose composition decides whether the split is available.
struct AbiDoc {
    clay_document* d = nullptr;
    clay_layer_id below = 0, active = 0;
    AbiDoc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "below", &below) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "active", &active) == CLAY_OK);
    }
    ~AbiDoc() { clay_document_destroy(d); }
    AbiDoc(const AbiDoc&) = delete;
    AbiDoc& operator=(const AbiDoc&) = delete;
};

void add_sphere(clay_document* d, clay_layer_id layer, float r, float x) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_layer_add_item(d, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
}

void add_sphere_to(AbiDoc& doc, clay_layer_id layer, float r, float x) {
    add_sphere(doc.d, layer, r, x);
}

std::vector<clay_brick_request> equator_bricks() {
    constexpr int kDim = 8;
    constexpr float kVox = 0.05f;
    constexpr int kBricks = 8;
    std::vector<clay_brick_request> reqs(kBricks);
    for (int i = 0; i < kBricks; ++i) {
        std::memset(&reqs[i], 0, sizeof(reqs[i]));
        const int kx = i - kBricks / 2;
        reqs[i].key[0] = kx;
        reqs[i].key[1] = -1;
        reqs[i].key[2] = -1;
        reqs[i].origin[0] = static_cast<float>(kx) * kDim * kVox;
        reqs[i].origin[1] = -1.0f * kDim * kVox;
        reqs[i].origin[2] = -1.0f * kDim * kVox;
        reqs[i].spacing = kVox;
        reqs[i].dims[0] = kDim;
        reqs[i].dims[1] = kDim;
        reqs[i].dims[2] = kDim;
        reqs[i].band = 3.0f * kVox;
    }
    return reqs;
}

std::vector<float> refill(clay_document* d, const std::vector<clay_brick_request>& reqs) {
    const std::size_t per = 8 * 8 * 8;
    std::vector<float> out(reqs.size() * per, 0.0f);
    REQUIRE(clay_brick_cache_eval_requests(d, nullptr, reqs.data(), reqs.size(), out.data(),
                                           out.size(), nullptr, 0) == CLAY_OK);
    bool near_surface = false;
    for (float v : out) near_surface = near_surface || std::fabs(v) < 0.5f;
    REQUIRE(near_surface);  // or every comparison is two readings of "far outside"
    return out;
}

std::uint64_t resumed_bricks(const clay_document* d) {
    clay_resume_stats s{};
    s.struct_size = sizeof s;
    REQUIRE(clay_document_resume_stats(d, &s) == CLAY_OK);
    return s.resumed_bricks;
}

// The two-layer stroke fixture, built from scratch at `dabs` dabs so a
// resumed answer can be compared against one that never resumed anything.
void build_stroke(AbiDoc& doc, int dabs, int op) {
    add_sphere_to(doc, doc.below, 1.0f, 0.0f);
    add_sphere_to(doc, doc.below, 0.45f, 0.85f);
    REQUIRE(clay_document_set_layer_composition(doc.d, doc.active, op, CLAY_BLEND_HARD, 0.0f,
                                                0.0f) == CLAY_OK);
    add_sphere_to(doc, doc.active, 0.35f, 0.9f);
    for (int i = 1; i <= dabs; ++i)
        add_sphere_to(doc, doc.active, 0.30f, 1.05f - 0.03f * static_cast<float>(i));
}

}  // namespace

// The same verdict WHERE A HOST MEETS IT. design.md 11 asks for the regression
// test at `clay_layer_placement_report`, not one level below it: the classifier
// is internal, and what a host acts on is the report -- the call that decides
// whether clay_layer_placement_begin/_update/_commit may skip a refill. A test
// that only held `layer_scales_cleanly` would keep passing if the report ever
// stopped consulting it.
TEST_CASE("c abi: a composed layer's radius is reported to the host as GENERAL") {
    clay_document* d = clay_document_create();
    REQUIRE(d != nullptr);
    clay_layer_id base = 0, cutter = 0;
    REQUIRE(clay_add_sdf_layer(d, "base", &base) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(d, "cutter", &cutter) == CLAY_OK);
    add_sphere(d, base, 1.0f, 0.0f);
    add_sphere(d, cutter, 0.5f, 0.6f);  // hard items: nothing but the fold can be wrong

    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    auto kind_of_a_doubling = [&]() {
        clay_placement_report rep;
        std::memset(&rep, 0, sizeof rep);
        rep.struct_size = static_cast<std::uint32_t>(sizeof rep);
        REQUIRE(clay_layer_placement_report(d, cutter, pos, axis, 0.0f, 2.0f, nullptr, &rep) ==
                CLAY_OK);
        return rep.kind;
    };

    // The control, and it is what makes the rest a claim rather than a
    // tautology: a hard-unioning layer of hard items IS a similarity of its own
    // field, so the cheap path is available and this call says so.
    CHECK(kind_of_a_doubling() == CLAY_PLACEMENT_SIMILARITY);

    SUBCASE("a soft fold radius takes the cheap path away") {
        REQUIRE(clay_document_set_layer_composition(d, cutter, CLAY_OP_SUBTRACT,
                                                    CLAY_BLEND_QUADRATIC, 0.2f, 0.0f) == CLAY_OK);
        CHECK(kind_of_a_doubling() == CLAY_PLACEMENT_GENERAL);
    }

    SUBCASE("and so does an EXTENDED one with a hard profile") {
        REQUIRE(clay_document_set_layer_composition(d, cutter, CLAY_OP_GROOVE, CLAY_BLEND_HARD,
                                                    0.15f, 0.0f) == CLAY_OK);
        CHECK(kind_of_a_doubling() == CLAY_PLACEMENT_GENERAL);
    }

    SUBCASE("and so does a hard-profile PAINT, which is below the extended range") {
        // The op a predicate spelled as "soft profile OR extended" cannot see:
        // CLAY_OP_PAINT is 3 and the extended range starts at 4, so a hard
        // paint with a positive radius fell through both clauses and the host
        // was told SIMILARITY. Its colour fades over exactly blend_k --
        // `cmax(blend_support(profile, k), k)` -- so the radius is as absolute
        // as any other, and the engine already pads the document for it.
        REQUIRE(clay_document_set_layer_composition(d, cutter, CLAY_OP_PAINT, CLAY_BLEND_HARD,
                                                    0.3f, 0.0f) == CLAY_OK);
        CHECK(kind_of_a_doubling() == CLAY_PLACEMENT_GENERAL);
    }

    SUBCASE("and so does a hard boolean carrying one, because the cull pad reads it") {
        // Conservative on purpose. A hard Add ignores blend_k in the FIELD, so
        // this layer really is a similarity of its own field -- but
        // `layer_blend_support` is `cmax(blend.support(), blend.k)` for every
        // op, so 0.3 reaches `clay_document_cull_pad` and the engine is already
        // treating it as a world radius. Two answers to one question is the
        // failure this change exists to remove, so the verdict follows the pad.
        REQUIRE(clay_document_set_layer_composition(d, cutter, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.3f,
                                                    0.0f) == CLAY_OK);
        CHECK(kind_of_a_doubling() == CLAY_PLACEMENT_GENERAL);
    }

    SUBCASE("the fold's ROUNDING does follow the scale, so it keeps the cheap path") {
        REQUIRE(clay_document_set_layer_composition(d, cutter, CLAY_OP_SUBTRACT, CLAY_BLEND_HARD,
                                                    0.0f, 0.2f) == CLAY_OK);
        CHECK(kind_of_a_doubling() == CLAY_PLACEMENT_SIMILARITY);
    }

    clay_document_destroy(d);
}

TEST_CASE("refill: a composed top layer refuses the split and still answers the field") {
    const std::vector<clay_brick_request> reqs = equator_bricks();

    auto fresh = [&](int dabs, int op) {
        AbiDoc d;
        build_stroke(d, dabs, op);
        return refill(d.d, reqs);
    };

    SUBCASE("a unioning top layer resumes, which is what makes this a comparison") {
        AbiDoc doc;
        build_stroke(doc, 0, CLAY_OP_ADD);
        refill(doc.d, reqs);  // takes the seeds
        const std::uint64_t before = resumed_bricks(doc.d);
        add_sphere_to(doc, doc.active, 0.30f, 1.02f);
        refill(doc.d, reqs);
        CHECK(resumed_bricks(doc.d) > before);
    }

    SUBCASE("a subtracting top layer resumes NOTHING, dab after dab") {
        AbiDoc doc;
        build_stroke(doc, 0, CLAY_OP_SUBTRACT);
        const std::vector<float> first = refill(doc.d, reqs);
        CHECK(first == fresh(0, CLAY_OP_SUBTRACT));
        for (int i = 1; i <= 4; ++i) {
            CAPTURE(i);
            const std::uint64_t before = resumed_bricks(doc.d);
            add_sphere_to(doc, doc.active, 0.30f, 1.05f - 0.03f * static_cast<float>(i));
            const std::vector<float> got = refill(doc.d, reqs);
            // A COUNT, not a clock: the refusal is invisible in the values by
            // contract, so this is the only thing that can see it.
            CHECK(resumed_bricks(doc.d) == before);
            // ...and the answer is still the field, which is the half that
            // matters. A rejoin with the wrong operator would pass the count.
            const std::vector<float> want = fresh(i, CLAY_OP_SUBTRACT);
            REQUIRE(got.size() == want.size());
            CHECK(std::memcmp(got.data(), want.data(), got.size() * sizeof(float)) == 0);
        }
    }

    SUBCASE("the refill's field is the FOLD's, checked against a document that never folds") {
        // A refill compared against another refill cannot see a wrong rejoin:
        // both halves of the comparison would be wrong the same way. So the
        // oracle is a ONE-LAYER document -- which takes no split at all, having
        // nothing beneath -- expressing the same shape as items. A hard
        // subtract is associative in exactly the way that makes the two forms
        // the same field: max(acc, -a, -b) is acc minus (a union b).
        AbiDoc composed_doc;
        build_stroke(composed_doc, 3, CLAY_OP_SUBTRACT);
        const std::vector<float> got = refill(composed_doc.d, reqs);

        clay_document* one = clay_document_create();
        REQUIRE(one != nullptr);
        clay_layer_id only = 0;
        REQUIRE(clay_add_sdf_layer(one, "only", &only) == CLAY_OK);
        auto item = [&](float r, float x, int32_t op) {
            clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
            REQUIRE(it != nullptr);
            const float pos[3] = {x, 0.0f, 0.0f};
            REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
            clay_node_id id = 0;
            REQUIRE(clay_layer_add_item(one, only, it, &id) == CLAY_OK);
            clay_item_destroy(it);
            REQUIRE(clay_layer_set_op_blend(one, only, id, op, CLAY_BLEND_HARD, 0.0f, 0.0f) ==
                    CLAY_OK);
        };
        item(1.0f, 0.0f, CLAY_OP_ADD);
        item(0.45f, 0.85f, CLAY_OP_ADD);
        item(0.35f, 0.9f, CLAY_OP_SUBTRACT);
        for (int i = 1; i <= 3; ++i)
            item(0.30f, 1.05f - 0.03f * static_cast<float>(i), CLAY_OP_SUBTRACT);
        const std::vector<float> want = refill(one, reqs);
        clay_document_destroy(one);

        REQUIRE(got.size() == want.size());
        CHECK(std::memcmp(got.data(), want.data(), got.size() * sizeof(float)) == 0);
    }

    SUBCASE("and the composition really does change these bricks") {
        // Teeth: without this the case above would hold for a document whose
        // composed layer reached none of the samples.
        CHECK(fresh(2, CLAY_OP_SUBTRACT) != fresh(2, CLAY_OP_ADD));
    }

    SUBCASE("a seed taken while the layers unioned is not carried forward by a plan") {
        // A document that UNIONED when its seeds were taken and composes now
        // still holds them, in every brick the composition change's dirty
        // region did not reach. Nothing may carry one of those forward and
        // rejoin its two halves with a hard Add.
        //
        // WHAT THIS DOES AND DOES NOT ISOLATE, because the difference matters
        // to whoever reads a failure here. Three refusals stand between this
        // and a wrong field -- plan_resume's, plan_frontier's and the store's
        // in eval_requests_impl -- and each is invisible while the others hold:
        // removing the store's leaves the plans refusing, and removing the
        // plans' leaves no seed for them to refuse. Reverting all three is what
        // moves this count. The append path additionally cannot reach the plan
        // at all here, because the same invalidation that dirties the region
        // retires the append log; the route that survives that is a FRONTIER
        // seed, which a composition change deliberately does not invalidate
        // (it moves no root ordinals, so it is not structural), and
        // plan_frontier's refusal is what closes it.
        //
        // The count is the whole of the claim, deliberately. Where the dirty
        // region is right the stale seeds sit in bricks the new operator does
        // not reach, so the VALUES agree either way and a field comparison
        // could not see this.
        AbiDoc doc;
        add_sphere_to(doc, doc.below, 1.0f, 0.0f);
        add_sphere_to(doc, doc.active, 0.35f, 0.9f);
        refill(doc.d, reqs);  // seeds taken while both layers union
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.active, CLAY_OP_SUBTRACT,
                                                    CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_OK);
        add_sphere_to(doc, doc.active, 0.30f, 1.02f);  // an append, so a plan has a suffix
        const std::uint64_t before = resumed_bricks(doc.d);
        refill(doc.d, reqs);
        CHECK(resumed_bricks(doc.d) == before);
    }

    SUBCASE("a composed layer BENEATH keeps the split") {
        // The seam is the last visible SDF layer alone: a cutter underneath the
        // layer being sculpted is the ordinary shape of this feature, and it
        // must not cost the fast path.
        AbiDoc doc;
        add_sphere_to(doc, doc.below, 1.0f, 0.0f);
        add_sphere_to(doc, doc.active, 0.35f, 0.9f);
        clay_layer_id third = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "top", &third) == CLAY_OK);
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.active, CLAY_OP_SUBTRACT,
                                                    CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_OK);
        add_sphere_to(doc, third, 0.5f, 0.2f);

        refill(doc.d, reqs);
        const std::uint64_t before = resumed_bricks(doc.d);
        add_sphere_to(doc, third, 0.3f, 0.35f);
        refill(doc.d, reqs);
        CHECK(resumed_bricks(doc.d) > before);
    }
}

// -- 4.3 at the ABI: the excluding calls refuse rather than mislead -----------

TEST_CASE("c abi: excluding a layer is refused once any layer composes") {
    AbiDoc doc;
    add_sphere_to(doc, doc.below, 1.0f, 0.0f);
    add_sphere_to(doc, doc.active, 0.5f, 0.8f);
    const float pts[6] = {0.0f, 0.0f, 0.0f, 0.9f, 0.0f, 0.0f};
    float d[2] = {0.0f, 0.0f};
    float g[6] = {0.0f};
    const std::vector<clay_brick_request> reqs = equator_bricks();
    std::vector<float> bricks(reqs.size() * 8 * 8 * 8, 0.0f);

    SUBCASE("while every layer unions it answers, which is the promise it kept") {
        CHECK(clay_eval_points_excluding(doc.d, doc.active, nullptr, pts, 2, d, nullptr) ==
              CLAY_OK);
        CHECK(clay_eval_gradients_excluding(doc.d, doc.active, nullptr, pts, 2, g) == CLAY_OK);
        CHECK(clay_brick_cache_eval_requests_excluding(doc.d, doc.active, nullptr, reqs.data(),
                                                       reqs.size(), bricks.data(), bricks.size(),
                                                       nullptr, 0) == CLAY_OK);
    }

    SUBCASE("a composed layer refuses all three, and names the layer") {
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.active, CLAY_OP_SUBTRACT,
                                                    CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_OK);
        CHECK(clay_eval_points_excluding(doc.d, doc.active, nullptr, pts, 2, d, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        const std::string why = clay_last_error() ? clay_last_error() : "";
        CHECK(why.find(std::to_string(doc.active)) != std::string::npos);
        CHECK(clay_eval_gradients_excluding(doc.d, doc.active, nullptr, pts, 2, g) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_eval_requests_excluding(doc.d, doc.active, nullptr, reqs.data(),
                                                       reqs.size(), bricks.data(), bricks.size(),
                                                       nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("the refusal is about the FOLD, not about which layer is excluded") {
        // Excluding the composed layer is refused, and so is excluding any
        // other: the parts of a folded document do not compose either way.
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.active, CLAY_OP_SUBTRACT,
                                                    CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_OK);
        CHECK(clay_eval_points_excluding(doc.d, doc.below, nullptr, pts, 2, d, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a composition on the FIRST visible layer is not applied, so it refuses nothing") {
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.below, CLAY_OP_INTERSECT,
                                                    CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_OK);
        CHECK(clay_eval_points_excluding(doc.d, doc.active, nullptr, pts, 2, d, nullptr) ==
              CLAY_OK);
    }
}

// -- 5.3, the half that was missing: the reach a fold gives an edit -----------

namespace {

// EVERY POINT THE COMMAND CHANGED, AGAINST THE BOX THE COMMAND CLAIMS.
//
// This is the invariant apply_edit rests on and it is the only one that
// matters: a brick outside `bound` keeps the seed it already holds AND has its
// revision advanced, so the next refill answers it from the stale seed through
// the rev == now path. A sample that moved outside the box is a brick that will
// never be recomputed and will never report anything.
//
// BAND-CLAMPED, because that is what a brick stores. A difference far outside
// the narrow band is not something any consumer can see; a difference inside it
// is exactly the surface moving.
//
// AND THE BOX IS DILATED BY THE BAND, because a brick is refilled when its own
// box INTERSECTS the dirty region, not when its samples are inside it: a sample
// within a band of the region sits in a brick that touches it. Without this the
// check would fail on a plain unioning document, where hiding a layer moves the
// band a hair outside that layer's geometry box -- which is why the control
// subcases below run the same measurement over a document that unions.
constexpr float kBand = 0.1f;

int changed_outside(const Document& before, const Document& after, const math::Aabb& bound,
                    const std::vector<cfloat3>& pts, float* worst = nullptr) {
    const Tape a = compile_document(before);
    const Tape b = compile_document(after);
    const math::Aabb box = bound.empty() ? bound : bound.dilated(kBand);
    int n = 0;
    if (worst) *worst = 0.0f;
    for (cfloat3 p : pts) {
        const float da = kernel::cclamp(a.eval(p).d, -kBand, kBand);
        const float db = kernel::cclamp(b.eval(p).d, -kBand, kBand);
        if (da == db) continue;
        if (box.contains(p)) continue;
        ++n;
        if (worst) *worst = kernel::cmax(*worst, std::fabs(da - db));
    }
    return n;
}

// The bound apply_edit actually uses: command_influence_bound on BOTH sides of
// the apply, unioned. Neither side alone is the answer -- an add's layer is not
// there before and a removal's is not there after -- which is the contract
// command_influence_bound states and apply_edit follows.
math::Aabb reach_of(const Document& before, const Document& after, const Command& cmd) {
    math::Aabb b = command_influence_bound(before, cmd);
    b.expand(command_influence_bound(after, cmd));
    return b;
}

// The document a command produces.
//
// ONLY FOR LAYER-LEVEL COMMANDS. Copying a Document shares its SdfContent by
// shared_ptr, so a command that edits an ITEM edits both copies and every
// comparison is then against itself -- silently passing. The item cases below
// build their two documents from scratch for that reason, which also makes them
// independent of whether `apply` did what it said.
Document applied(const Document& doc, const Command& cmd) {
    Document after = doc;
    REQUIRE(apply(after, cmd).has_value());
    return after;
}

}  // namespace

TEST_CASE("hiding the bottom layer dirties the layer it stops being folded into") {
    // THE FIRST-VISIBLE FLIP. The first visible SDF layer initialises the
    // accumulator and its own operator is not applied, so hiding, removing,
    // adding or reordering the BOTTOM layer turns the layer above it from
    // folded into initialising -- and a subtractive cutter that was taking
    // material away comes back as the base shape, solid, over its OWN whole
    // extent. The edited layer's box is nowhere near it.
    Document doc;
    Layer& base = doc.add_sdf_layer("base");
    base.sdf->insert(sphere_at(0.0f, 1.0f));
    const LayerId base_id = base.id;
    Layer& cutter = doc.add_sdf_layer("cutter");
    cutter.sdf->insert(sphere_at(1.3f, 0.5f));
    cutter.composition = composed(Op::Subtract);

    const std::vector<cfloat3> pts = lattice(24);
    const Command hide{SetLayerVisibleCmd{base_id, false}};
    const Document hidden = applied(doc, hide);
    const math::Aabb reach = reach_of(doc, hidden, hide);
    float worst = 0.0f;
    CHECK(changed_outside(doc, hidden, reach, pts, &worst) == 0);
    CHECK(worst == 0.0f);

    SUBCASE("and the flip really does change the field out there") {
        // The teeth, twice over: the point moves, and it moves OUTSIDE the box
        // the base layer alone would have named -- so the widening is what is
        // being tested and not an accident of the fixture.
        const cfloat3 p = cf3(1.6f, 0.0f, 0.0f);
        const float lit = compile_document(doc).eval(p).d;
        const float dark = compile_document(hidden).eval(p).d;
        CHECK(lit > 0.0f);   // the cutter is carving here, so there is no material
        CHECK(dark < 0.0f);  // ...and with the base gone it IS the material
        CHECK_FALSE(layer_influence_bound(*doc.find_layer(base_id)).contains(p));
        CHECK(reach.contains(p));
    }

    SUBCASE("removing it and adding it back name the same region") {
        const Command remove{RemoveLayerCmd{base_id}};
        const Document without = applied(doc, remove);
        CHECK(changed_outside(doc, without, reach_of(doc, without, remove), pts) == 0);
        // ...and back, which with the removal is the reorder gate's Remove+Add
        // pair: each half has to name the flip on its own side.
        AddLayerCmd add;
        add.layer = *doc.find_layer(base_id);
        add.index = 0;
        const Command re{add};
        const Document restored = applied(without, re);
        CHECK(changed_outside(without, restored, reach_of(without, restored, re), pts) == 0);
    }

    SUBCASE("a unioning layer above needs none of it, so nothing that unions pays") {
        Document plain = doc;
        plain.layers.back().composition = LayerComposition{};
        const math::Aabb b = command_influence_bound(plain, hide);
        const math::Aabb own = layer_influence_bound(*plain.find_layer(base_id));
        CHECK(b.min.x == doctest::Approx(own.min.x));
        CHECK(b.max.x == doctest::Approx(own.max.x));
    }
}

TEST_CASE("an item edit is dilated by the folds it passes through") {
    // A combine is POINTWISE, so an edit beneath a carving layer changes the
    // document exactly where it changed the accumulator -- but a SMOOTH fold is
    // not pointwise: it moves the result up to its own support away from where
    // its operands moved. That is the dilation node_reach_bound applies once per
    // enclosing GROUP, one level up; node_reach_bound stops at the layer root
    // because it holds a Layer and not a Document, so this is the term only
    // node_command_bound can add.
    //
    // This is ORDINARY SCULPTING. Every dab into a document with one soft fold
    // leaves stale bricks without it -- both a dab into the folding layer and a
    // dab into the layer beneath it, which is why both are measured here.
    const std::vector<cfloat3> pts = lattice(32);

    // Built from scratch on each side rather than copied and edited: a Document
    // copy shares its SdfContent, so an item edit through `apply` would move
    // both sides at once and the comparison would pass by being against itself.
    auto build = [](float low_r, float high_r, float k) {
        Document d;
        d.add_sdf_layer("low").sdf->insert(sphere_at(-0.7f, low_r));
        Layer& h = d.add_sdf_layer("high");
        h.sdf->insert(sphere_at(0.7f, high_r));
        h.composition = composed(Op::Add, BlendProfile::Quadratic, k);
        return d;
    };

    for (bool edit_below : {false, true}) {
        CAPTURE(edit_below);
        const Document before = build(0.5f, 0.5f, 0.15f);
        const Document after =
            edit_below ? build(0.58f, 0.5f, 0.15f) : build(0.5f, 0.58f, 0.15f);
        const Layer& target = edit_below ? before.layers.front() : before.layers.back();
        const Command grow{SetPrimCmd{target.id, target.sdf->roots.back(), Prim::sphere(0.58f)}};

        const math::Aabb reach = reach_of(before, after, grow);
        float worst = 0.0f;
        CHECK(changed_outside(before, after, reach, pts, &worst) == 0);
        CHECK(worst == 0.0f);

        SUBCASE("and the fold's support is exactly what the box was missing") {
            // The revert, measured rather than described: the same box without
            // the layer fold's dilation is what `node_reach_bound` alone
            // reports, and the field leaves it.
            const float support = layer_blend_support(before.layers.back());
            REQUIRE(support == doctest::Approx(0.6f));
            CHECK(changed_outside(before, after, reach.dilated(-support), pts) > 0);
        }

        SUBCASE("and a HARD fold pays none of it") {
            // The control. Under a hard union the same edit stays inside the
            // un-dilated box, so the leak above is the fold's support and not
            // the fixture's geometry -- and every document that predates layer
            // composition is this one.
            const Document hb = build(0.5f, 0.5f, 0.0f);
            const Document ha = edit_below ? build(0.58f, 0.5f, 0.0f) : build(0.5f, 0.58f, 0.0f);
            const Layer& t = edit_below ? hb.layers.front() : hb.layers.back();
            const Command g{SetPrimCmd{t.id, t.sdf->roots.back(), Prim::sphere(0.58f)}};
            CHECK(layer_blend_support(hb.layers.back()) == 0.0f);
            CHECK(changed_outside(hb, ha, reach_of(hb, ha, g), pts) == 0);
        }
    }
}


// -- blockers 2, 3 and 4: every HOST-FACING route to "where an edit reaches" --
//
// design.md 13b names the exact call path a real host takes:
//
//     node_bound     -> Document::node_influence_bound -> clay_layer_node_influence_bound
//     refill_region  -> BrickCache::mark_dirty         -> clay_brick_cache_mark_dirty
//
// so the QUERY is the surface that has to be right and the dirty call is
// downstream of it and blameless. The first fix for the fold widened only the
// internal command path, which left all four host-facing routes -- the two
// influence-bound queries and the two mark_dirty calls -- reporting the
// UN-DILATED box, and left all three GESTURE reaches un-dilated as well,
// because a gesture states its reach itself and never passes through
// command_influence_bound. A single stamp issued through apply_edit got the
// widening; the same stamp issued as a stroke did not, and that is ordinary
// sculpting.
//
// WHY EVERY CASE HERE COMPARES A FIELD RATHER THAN A BOX. A box that is too
// small has nothing wrong with it to look at: it is a plausible box, and an
// assertion against another box is an assertion against a second opinion. The
// only statement worth holding is the one the box is FOR -- every point the
// edit changed is inside it -- so each case samples the document either side of
// the edit and counts the band-clamped changes that landed outside what the
// host was handed.
//
// BAND-CLAMPED, which is the sense every bound in this tree is conservative in.
// A point whose |distance| exceeds the band is not one a brick stores, so a
// change there is not one a refill would ever serve; and a point INSIDE the
// band is within `band` of a surface, so a box that contains the surface
// contains the band around it once dilated by the band. That dilation is what a
// brick cache already does for itself (BrickCache::mark_dirty), and it is
// applied by hand here where the box is compared raw.

namespace {

constexpr float kFoldK = 0.15f;  // quadratic: support 4k = 0.6
constexpr float kFoldSupport = 4.0f * kFoldK;

// TWO VISIBLE SDF LAYERS, THE EDIT IN THE LOWER ONE, A SMOOTH FOLD ABOVE IT.
//
// `lower` is the first visible SDF layer, so its OWN composition is never
// applied and the only fold in the document is `upper`'s -- the dilation every
// case below is about is exactly one term, 0.6, and a failure cannot be read as
// some other layer's.
//
// The two shells are placed so their fields are within the fold's support of
// each other over a wide region: that is where a smooth fold moves the
// document's surface at points where the LAYER's own field says nothing moved,
// which is the whole escape.
struct ReachDoc {
    clay_document* d = nullptr;
    clay_layer_id lower = 0, upper = 0;
    clay_node_id blob = 0;

    explicit ReachDoc(float blob_x = 0.95f, float k = kFoldK) {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "lower", &lower) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "upper", &upper) == CLAY_OK);
        add(lower, 1.0f, 0.0f, nullptr);
        add(lower, 0.35f, blob_x, &blob);
        add(upper, 0.6f, 1.5f, nullptr);
        REQUIRE(clay_document_set_layer_composition(d, upper, CLAY_OP_ADD,
                                                    k > 0.0f ? CLAY_BLEND_QUADRATIC
                                                             : CLAY_BLEND_HARD,
                                                    k, 0.0f) == CLAY_OK);
    }
    ~ReachDoc() { clay_document_destroy(d); }
    ReachDoc(const ReachDoc&) = delete;
    ReachDoc& operator=(const ReachDoc&) = delete;

    void add(clay_layer_id layer, float r, float x, clay_node_id* out) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        const float pos[3] = {x, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(d, layer, it, out) == CLAY_OK);
        clay_item_destroy(it);
    }
};

// The sample lattice: through the seam between the two layers and out past both
// of them, fine enough that a 0.6-wide escape shell holds hundreds of points.
std::vector<float> seam_points() {
    std::vector<float> pts;
    for (int i = 0; i <= 44; ++i)
        for (int j = 0; j <= 22; ++j)
            for (int k = 0; k <= 22; ++k) {
                pts.push_back(-0.6f + 0.07f * static_cast<float>(i));
                pts.push_back(-0.77f + 0.07f * static_cast<float>(j));
                pts.push_back(-0.77f + 0.07f * static_cast<float>(k));
            }
    return pts;
}

std::vector<float> abi_eval(const clay_document* d, const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3, 0.0f);
    REQUIRE(clay_eval_points(d, "cpu", pts.data(), out.size(), out.data(), nullptr) == CLAY_OK);
    return out;
}

// A box as the C ABI hands one back, and the union a host takes across an edit
// -- which is what design.md 13b says place_layer and set_object_transform do.
struct Box {
    float lo[3]{0, 0, 0};
    float hi[3]{0, 0, 0};
    bool has = false;
    bool infinite = false;

    void unite(const Box& o) {
        infinite = infinite || o.infinite;
        if (!o.has) return;
        if (!has) {
            *this = o;
            return;
        }
        for (int a = 0; a < 3; ++a) {
            lo[a] = kernel::cmin(lo[a], o.lo[a]);
            hi[a] = kernel::cmax(hi[a], o.hi[a]);
        }
    }
    bool contains(const float* p, float pad) const {
        if (infinite) return true;
        if (!has) return false;
        for (int a = 0; a < 3; ++a)
            if (p[a] < lo[a] - pad || p[a] > hi[a] + pad) return false;
        return true;
    }
};

Box node_query(const clay_document* d, clay_layer_id layer, clay_node_id node) {
    Box b;
    std::int32_t has = 0, inf = 0;
    REQUIRE(clay_layer_node_influence_bound(d, layer, node, b.lo, b.hi, &has, &inf) == CLAY_OK);
    b.has = has != 0;
    b.infinite = inf != 0;
    return b;
}

Box layer_query(const clay_document* d, clay_layer_id layer) {
    Box b;
    std::int32_t has = 0, inf = 0;
    REQUIRE(clay_layer_influence_bound(d, layer, b.lo, b.hi, &has, &inf) == CLAY_OK);
    b.has = has != 0;
    b.infinite = inf != 0;
    return b;
}

// How many sampled points the edit moved OUTSIDE the box, and by how much. A
// count and a value, never a clock; `worst` is reported because a failure needs
// to say how far past the box the field went, not only that it did.
struct Escape {
    int outside = 0;
    int changed = 0;
    float worst = 0.0f;
};

Escape escaped(const std::vector<float>& before, const std::vector<float>& after,
               const std::vector<float>& pts, const Box& box, float pad) {
    Escape e;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const bool in_band = std::fabs(before[i]) <= kBand || std::fabs(after[i]) <= kBand;
        const float delta = std::fabs(after[i] - before[i]);
        // 1e-4 rather than 0: a quadratic smin's deviation dies quadratically
        // into its own support, so the last hair of the shell moves by an
        // amount no brick could store. The escapes this exists to catch are
        // measured at 0.05, five hundred times that.
        if (!in_band || delta <= 1e-4f) continue;
        ++e.changed;
        if (box.contains(&pts[i * 3], pad)) continue;
        ++e.outside;
        e.worst = kernel::cmax(e.worst, delta);
    }
    return e;
}

// A brick cache small enough that the box it marks is close to the box it was
// given: dim 8 at 0.05 is a 0.4 brick with a 0.15 band.
clay_brick_cache* reach_cache() {
    clay_brick_config c;
    std::memset(&c, 0, sizeof c);
    c.struct_size = static_cast<std::uint32_t>(sizeof c);
    c.dim = 8;
    c.voxel_size = 0.05f;
    c.band_voxels = 3;
    clay_brick_cache* cache = clay_brick_cache_create(&c);
    REQUIRE(cache != nullptr);
    return cache;
}

// What a mark actually dirtied, read back as the union of the bricks it queued.
// mark_dirty tracks keys it has not seen, so a fresh cache reports exactly the
// region it was handed -- dilated by its own band and snapped outward to the
// brick grid, which can only make this comparison more forgiving.
Box marked_box(clay_brick_cache* cache) {
    Box b;
    std::vector<clay_brick_request> reqs(4096);
    std::size_t remaining = 0;
    do {
        std::size_t count = reqs.size();
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        for (std::size_t i = 0; i < count; ++i) {
            Box one;
            one.has = true;
            for (int a = 0; a < 3; ++a) {
                one.lo[a] = reqs[i].origin[a];
                one.hi[a] = reqs[i].origin[a] +
                            static_cast<float>(reqs[i].dims[a]) * reqs[i].spacing;
            }
            b.unite(one);
        }
    } while (remaining > 0);
    return b;
}

// The edit every case makes: slide the small sphere in the LOWER layer. Small
// enough to be one dab of a stroke, and inside the layer whose value the fold
// above drags.
void slide_blob(ReachDoc& doc, float to_x) {
    const float pos[3] = {to_x, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};  // a NULL axis is refused, not "unrotated"
    REQUIRE(clay_layer_set_transform(doc.d, doc.lower, doc.blob, pos, axis, 0.0f, 1.0f) ==
            CLAY_OK);
}

}  // namespace

TEST_CASE("the four host-facing bounds cover everything an edit under a fold changes") {
    const std::vector<float> pts = seam_points();

    SUBCASE("clay_layer_node_influence_bound, which is what a host dirties by") {
        ReachDoc doc;
        const std::vector<float> before = abi_eval(doc.d, pts);
        Box box = node_query(doc.d, doc.lower, doc.blob);
        slide_blob(doc, 1.05f);
        box.unite(node_query(doc.d, doc.lower, doc.blob));  // union(before, after)
        const std::vector<float> after = abi_eval(doc.d, pts);

        const Escape e = escaped(before, after, pts, box, kBand);
        CAPTURE(e.worst);
        CHECK(e.changed > 100);  // teeth: the edit is visible in the band at all
        CHECK(e.outside == 0);
    }

    SUBCASE("clay_brick_cache_mark_dirty_nodes, the documented default") {
        ReachDoc doc;
        clay_brick_cache* cache = reach_cache();
        const std::vector<float> before = abi_eval(doc.d, pts);
        REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.lower, &doc.blob, 1,
                                                  nullptr) == CLAY_OK);
        slide_blob(doc, 1.05f);
        REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.lower, &doc.blob, 1,
                                                  nullptr) == CLAY_OK);
        const std::vector<float> after = abi_eval(doc.d, pts);
        const Box box = marked_box(cache);
        clay_brick_cache_destroy(cache);

        // No pad: the cache dilated by its own band already, which is wider
        // than the band these samples are clamped at.
        const Escape e = escaped(before, after, pts, box, 0.0f);
        CAPTURE(e.worst);
        CHECK(e.changed > 100);
        CHECK(e.outside == 0);
    }

    SUBCASE("clay_layer_influence_bound") {
        ReachDoc doc;
        const std::vector<float> before = abi_eval(doc.d, pts);
        Box box = layer_query(doc.d, doc.lower);
        slide_blob(doc, 1.05f);
        box.unite(layer_query(doc.d, doc.lower));
        const std::vector<float> after = abi_eval(doc.d, pts);

        const Escape e = escaped(before, after, pts, box, kBand);
        CAPTURE(e.worst);
        CHECK(e.changed > 100);
        CHECK(e.outside == 0);
    }

    SUBCASE("clay_brick_cache_mark_dirty_layer, which is what a first full fill marks") {
        ReachDoc doc;
        clay_brick_cache* cache = reach_cache();
        const std::vector<float> before = abi_eval(doc.d, pts);
        REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.lower) == CLAY_OK);
        slide_blob(doc, 1.05f);
        REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.lower) == CLAY_OK);
        const std::vector<float> after = abi_eval(doc.d, pts);
        const Box box = marked_box(cache);
        clay_brick_cache_destroy(cache);

        const Escape e = escaped(before, after, pts, box, 0.0f);
        CAPTURE(e.worst);
        CHECK(e.changed > 100);
        CHECK(e.outside == 0);
    }

    SUBCASE("and a HARD fold pays for none of it, which every old document is") {
        // The control, and the teeth for all four: under a hard union the same
        // edit stays inside the un-dilated box, so what the four cases above
        // hold is the FOLD's support and not some property of the fixture. It
        // also pins the cost: a document that never sets a composition gets
        // exactly the box it always got.
        ReachDoc hard(0.95f, 0.0f);
        const Box before_box = node_query(hard.d, hard.lower, hard.blob);
        ReachDoc soft(0.95f, kFoldK);
        const Box soft_box = node_query(soft.d, soft.lower, soft.blob);
        CHECK(soft_box.hi[0] == doctest::Approx(before_box.hi[0] + kFoldSupport));
        CHECK(soft_box.lo[0] == doctest::Approx(before_box.lo[0] - kFoldSupport));

        const std::vector<float> before = abi_eval(hard.d, pts);
        Box box = before_box;
        slide_blob(hard, 1.05f);
        box.unite(node_query(hard.d, hard.lower, hard.blob));
        const std::vector<float> after = abi_eval(hard.d, pts);
        const Escape e = escaped(before, after, pts, box, kBand);
        // Fewer changed points than the folded cases above, and that IS the
        // control: a hard fold spreads nothing, so the only points that move
        // are the ones the layer's own field moved.
        CHECK(e.changed > 50);
        CHECK(e.outside == 0);
    }
}


// -- blocker 3: the three GESTURE reaches, which is the route an artist takes -

namespace {

// WHY A GESTURE IS NOT ASSERTED AGAINST A BOX, AND WHAT IS ASSERTED INSTEAD.
//
// A gesture does not report a region. `apply_edit_in_gesture` deliberately
// skips per-command invalidation and GestureRegion's destructor dirties `reach`
// ONCE for the whole stroke, so the reach is visible only in what it
// invalidated -- and its one consumer is the document's resume seed store
// (clay_document::touch_regions). GestureRegion's own contract is the sentence
// asserted here: "It MUST cover everything the bracket does -- a region that
// does not is stale bricks."
//
// So each case below takes a seed in a brick placed in the SHELL the fold adds
// -- outside the gesture's own ball, inside that ball dilated by the folds
// above the layer -- and holds that the gesture dropped it. A count, from
// clay_resume_stats, never a clock. A second brick far outside both is kept in
// the same window, so a gesture that simply dropped everything would fail too.
//
// WHAT THIS DOES NOT CLAIM, because the difference matters to whoever reads a
// failure here. It is NOT a demonstration of a stale brick, and the un-dilated
// reach does not produce one: `touch_region_locked` compares each seed's brick
// DILATED BY `band + pad`, and `pad` is the document cull pad -- a maximum over
// layers of that layer's chain pad PLUS the folds above it, so it is >= the
// fold sum this reach was missing, for EVERY document. Measured on this
// fixture over a 504-brick window (258,048 samples): with the gesture's
// dilation removed a drag leaves 288 seeds where the fixed one leaves 216, and
// the refill that follows is bit-identical to a cold document's -- 0 stale
// samples either way. So the seed store absorbed the shortfall, and it did so
// by ACCIDENT: the coverage belongs to the cull pad, it is not what
// GestureRegion promises, and the next consumer of a gesture's reach would not
// have it. The invalidation is what the contract is about, so the invalidation
// is what is measured.

// The 0.4 brick grain a cache uses, so the numbers below are the ones a real
// brick key produces.
constexpr float kVox = 0.05f;
constexpr int kDim = 8;
constexpr float kBrick = kDim * kVox;  // 0.4

clay_brick_request brick_at(int kx, int ky, int kz) {
    clay_brick_request r;
    std::memset(&r, 0, sizeof r);
    const int key[3] = {kx, ky, kz};
    for (int a = 0; a < 3; ++a) {
        r.key[a] = key[a];
        r.origin[a] = static_cast<float>(key[a]) * kBrick;
        r.dims[a] = kDim;
    }
    r.spacing = kVox;
    r.band = 3.0f * kVox;
    return r;
}

// THE TWO PROBE BRICKS, and the arithmetic that places them, because a brick
// chosen by eye would be a test that passes for a reason nobody wrote down.
//
// A seed is dropped when its brick DILATED BY band + pad intersects the reach:
// band 0.15 and pad 0.6 (the document cull pad, which is this document's one
// fold), so 0.75 either way. Every gesture below is aimed at (1.30, 0, 0) with
// a ball no wider than 0.17, so its own reach is at most x in [1.13, 1.47] and
// its fold-dilated reach is at least x in [0.58, 2.02].
//
//   SHELL brick key 6: x [2.40, 2.80], dilated [1.65, 3.55].
//     1.65 > 1.47, so the gesture's OWN ball never reaches it -- it survives
//     an un-dilated reach, which is the defect.
//     1.65 < 2.02, so the fold-dilated reach does -- it must be dropped.
//   CONTROL brick key 12: x [4.80, 5.20], dilated [4.05, 5.95]. Neither reach
//     comes near it, so it must survive: a gesture that dirtied everything
//     would pass the shell assertion and fail this one.
constexpr int kShellKey = 6;
constexpr int kControlKey = 12;

// THREE VISIBLE SDF LAYERS: the gesture's layer, a smooth fold above it, and a
// plain hard union above that.
//
// `top` is not decoration. A brick's seed is only STORED when the refill may
// hold the two halves apart, which is when the last visible SDF layer folds
// with a hard Add -- and it is only SERVED when the active half is not empty
// (`had_acc`), so `top` must have geometry in each probe brick or the case
// would pass by there being no seed to drop. Its two small spheres sit exactly
// in the two probe bricks and nowhere near the gesture.
struct GestureDoc {
    clay_document* d = nullptr;
    clay_layer_id lower = 0, mid = 0, top = 0;

    GestureDoc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "lower", &lower) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "mid", &mid) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "top", &top) == CLAY_OK);
        add(lower, 1.0f, 0.0f);
        add(lower, 0.35f, 0.95f);  // its front face is at x = 1.30
        add(mid, 0.6f, 1.5f);
        REQUIRE(clay_document_set_layer_composition(d, mid, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                                    kFoldK, 0.0f) == CLAY_OK);
        add(top, 0.2f, (static_cast<float>(kShellKey) + 0.5f) * kBrick);
        add(top, 0.2f, (static_cast<float>(kControlKey) + 0.5f) * kBrick);
    }
    ~GestureDoc() { clay_document_destroy(d); }
    GestureDoc(const GestureDoc&) = delete;
    GestureDoc& operator=(const GestureDoc&) = delete;

    void add(clay_layer_id layer, float r, float x) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        const float pos[3] = {x, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(d, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
    }
};

std::uint64_t seed_entries(const clay_document* d) {
    clay_resume_stats s{};
    s.struct_size = sizeof s;
    REQUIRE(clay_document_resume_stats(d, &s) == CLAY_OK);
    return s.entries;
}

// Fill the two probe bricks so each holds a seed, run the gesture, and report
// what is left. Both bricks in one call, so nothing about the order of the two
// fills can be the difference between them.
template <class Gesture>
void gesture_reach_carries_the_fold(const Gesture& run) {
    const clay_brick_request pair[2] = {brick_at(kShellKey, -1, -1),
                                        brick_at(kControlKey, -1, -1)};
    const std::size_t per = kDim * kDim * kDim;

    auto fill = [&](GestureDoc& doc) {
        std::vector<float> out(2 * per, 0.0f);
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, pair, 2, out.data(), out.size(),
                                               nullptr, 0) == CLAY_OK);
    };

    GestureDoc doc;
    fill(doc);
    // Both bricks hold one, or the assertions below are about nothing.
    REQUIRE(seed_entries(doc.d) == 2);
    run(doc);
    // EXACTLY ONE SURVIVES, which is two claims in one number: the SHELL
    // brick's seed is gone (2 would mean the reach stopped at the layer, which
    // is the defect) and the CONTROL brick's is not (0 would mean the gesture
    // dirtied the document rather than a region, which would pass a
    // one-sided assertion for the wrong reason).
    CHECK(seed_entries(doc.d) == 1);
}

clay_move_params drag_params() {
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.radius = 0.12f;
    return p;
}

clay_magnify_params swell_params() {
    clay_magnify_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.radius = 0.12f;
    return p;
}

}  // namespace

TEST_CASE("a gesture's reach carries the folds above its layer") {
    // All three are aimed at the lower layer's own surface: the 0.35 blob sits
    // at x = 0.95, so its front face is at 1.30.
    const float centre[3] = {1.30f, 0.0f, 0.0f};

    SUBCASE("clay_layer_move_surface") {
        const float pull[3] = {0.05f, 0.0f, 0.0f};
        const clay_move_params p = drag_params();
        gesture_reach_carries_the_fold([&](GestureDoc& doc) {
            std::size_t applied = 0;
            REQUIRE(clay_layer_move_surface(doc.d, doc.lower, centre, pull, &p, &applied) ==
                    CLAY_OK);
            REQUIRE(applied > 0);
        });
    }

    SUBCASE("clay_layer_magnify_surface") {
        const clay_magnify_params p = swell_params();
        gesture_reach_carries_the_fold([&](GestureDoc& doc) {
            std::size_t applied = 0;
            REQUIRE(clay_layer_magnify_surface(doc.d, doc.lower, centre, 0.35f, &p, &applied) ==
                    CLAY_OK);
            REQUIRE(applied > 0);
        });
    }

    SUBCASE("clay_layer_place_stamps, which is the stroke a dab is issued in") {
        // The blocker's own sentence: a single stamp issued through apply_edit
        // was dilated by command_influence_bound and the SAME stamp issued as a
        // stroke was not, because a stroke states its reach itself and never
        // passes through it.
        clay_document* src = clay_document_create();
        REQUIRE(src != nullptr);
        clay_layer_id sl = 0;
        REQUIRE(clay_add_sdf_layer(src, "src", &sl) == CLAY_OK);
        const float r = 0.2f;
        clay_item* ball = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(ball != nullptr);
        REQUIRE(clay_layer_add_item(src, sl, ball, nullptr) == CLAY_OK);
        clay_item_destroy(ball);

        const float hit[3] = {0.2f, 0.0f, 0.0f};
        const float normal[3] = {1.0f, 0.0f, 0.0f};
        clay_stamp_frame frame;
        std::memset(&frame, 0, sizeof frame);
        frame.struct_size = static_cast<std::uint32_t>(sizeof frame);
        REQUIRE(clay_stamp_frame_from_surface(hit, normal, 0.0f, &frame) == CLAY_OK);
        clay_volume_params vp;
        std::memset(&vp, 0, sizeof vp);
        vp.struct_size = static_cast<std::uint32_t>(sizeof vp);
        vp.cell_size = 0.02f;
        const float lo[3] = {-0.12f, -0.12f, -0.12f};
        const float hi[3] = {0.12f, 0.12f, 0.12f};
        clay_item* asset = nullptr;
        REQUIRE(clay_item_stamp_from_document(src, &vp, &frame, lo, hi, &asset, nullptr) ==
                CLAY_OK);
        REQUIRE(asset != nullptr);
        clay_document_destroy(src);

        std::vector<clay_stamp> dabs;
        for (int i = 0; i < 6; ++i) {
            clay_stamp s;
            std::memset(&s, 0, sizeof s);
            s.position[0] = centre[0];
            s.position[1] = -0.05f + 0.02f * static_cast<float>(i);
            s.position[2] = 0.0f;
            s.radius = 0.12f;
            s.rotation[3] = 1.0f;
            s.along = static_cast<float>(i);
            dabs.push_back(s);
        }

        gesture_reach_carries_the_fold([&](GestureDoc& doc) {
            std::size_t placed = 0;
            REQUIRE(clay_layer_place_stamps(doc.d, doc.lower, asset, dabs.data(), dabs.size(),
                                            nullptr, 0, &placed) == CLAY_OK);
            REQUIRE(placed == dabs.size());
        });
        clay_item_destroy(asset);
    }
}


// -- 12/12a: the THIRD half, which is what a folded document leaves a preview -
//
// A host previewing one layer per frame takes the rest of the document once, at
// pointer-down. While every layer unioned that was
// `clay_brick_cache_eval_requests_excluding` composed with a `min`. Once a layer
// composes the excluding form refuses -- the parts of a fold do not sum -- and
// the pairing that survives is BELOW + the previewed layer, rejoined under that
// layer's own composition. The claim is an EQUALITY over samples, not a
// tolerance: the split is taken at a layer boundary, so the below half is
// exactly the accumulator the whole-document walk holds when it reaches the top
// layer.

namespace {

constexpr std::size_t kBrickSamples = 8 * 8 * 8;

// Four SDF layers in the shape design.md 12a describes: a base, a cutter
// BENEATH the layer being previewed, a smoothly-added form, and a top layer
// whose composition the arms vary. The LOWER layers compose, which is the half
// the excluding form cannot answer at all and this one has to answer exactly.
struct BelowDoc {
    clay_document* d = nullptr;
    clay_layer_id base = 0, cutter = 0, form = 0, top = 0;

    BelowDoc(int32_t top_op, int32_t top_blend, float top_k, float top_rounding) {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "base", &base) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "cutter", &cutter) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "form", &form) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "top", &top) == CLAY_OK);
        add_sphere(d, base, 1.0f, 0.0f);
        add_sphere(d, cutter, 0.5f, -0.85f);
        add_sphere(d, form, 0.55f, 0.9f);
        add_sphere(d, top, 0.45f, 0.55f);
        add_sphere(d, top, 0.3f, -0.4f);
        REQUIRE(clay_document_set_layer_composition(d, cutter, CLAY_OP_SUBTRACT, CLAY_BLEND_HARD,
                                                    0.0f, 0.0f) == CLAY_OK);
        REQUIRE(clay_document_set_layer_composition(d, form, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                                    0.3f, 0.0f) == CLAY_OK);
        REQUIRE(clay_document_set_layer_composition(d, top, top_op, top_blend, top_k,
                                                    top_rounding) == CLAY_OK);
        // The comparison below is of FLOATS, so the whole-document arm is read
        // with the uniform gate off: a brick it proves uniform is answered with
        // a stand-in rather than with the field's samples, and the two scoped
        // halves are never gated at all.
        REQUIRE(clay_internal_set_uniform_gate(d, 0) == CLAY_OK);
    }
    ~BelowDoc() { clay_document_destroy(d); }
    BelowDoc(const BelowDoc&) = delete;
    BelowDoc& operator=(const BelowDoc&) = delete;
};

// Distance AND colour, because the combine couples them: a rejoin that got the
// operator right and the colour wrong is still not the document's field.
struct Field {
    std::vector<float> d, rgb;
};

Field make_field(std::size_t bricks) {
    Field f;
    f.d.assign(bricks * kBrickSamples, 0.0f);
    f.rgb.assign(bricks * kBrickSamples * 3, 0.0f);
    return f;
}

Field whole_field(clay_document* d, const std::vector<clay_brick_request>& reqs) {
    Field f = make_field(reqs.size());
    REQUIRE(clay_brick_cache_eval_requests(d, nullptr, reqs.data(), reqs.size(), f.d.data(),
                                           f.d.size(), f.rgb.data(), f.rgb.size()) == CLAY_OK);
    return f;
}

Field below_field(clay_document* d, clay_layer_id layer,
                  const std::vector<clay_brick_request>& reqs) {
    Field f = make_field(reqs.size());
    clay_layer_id blocking = 987654;
    std::uint32_t blocking_count = 987654;
    REQUIRE(clay_brick_cache_eval_requests_below(d, layer, nullptr, reqs.data(), reqs.size(),
                                                 f.d.data(), f.d.size(), f.rgb.data(),
                                                 f.rgb.size(), &blocking,
                                                 &blocking_count) == CLAY_OK);
    CHECK(blocking == 0);  // cleared on success, so a host cannot read a stale id
    CHECK(blocking_count == 0);
    return f;
}

Field layer_field(clay_document* d, clay_layer_id layer,
                  const std::vector<clay_brick_request>& reqs) {
    Field f = make_field(reqs.size());
    REQUIRE(clay_brick_cache_eval_requests_layer(d, layer, nullptr, reqs.data(), reqs.size(),
                                                 f.d.data(), f.d.size(), f.rgb.data(),
                                                 f.rgb.size()) == CLAY_OK);
    return f;
}

// What the HOST does per frame: fold the two halves it holds under the
// composition it read back for the previewed layer. `rounding` is passed as the
// stored value because every layer here carries an identity transform, where
// the world rounding the tape emits is the stored one; a scaled layer would
// need it multiplied by that layer's distance scale.
Field host_fold(const Field& below, const Field& active, int32_t op, int32_t blend, float k,
                float rounding) {
    Field out = make_field(below.d.size() / kBrickSamples);
    for (std::size_t s = 0; s < below.d.size(); ++s) {
        kernel::CTapeValue a;
        a.d = below.d[s];
        a.color = cf3(below.rgb[s * 3], below.rgb[s * 3 + 1], below.rgb[s * 3 + 2]);
        kernel::CTapeValue b;
        b.d = active.d[s];
        b.color = cf3(active.rgb[s * 3], active.rgb[s * 3 + 1], active.rgb[s * 3 + 2]);
        const kernel::CTapeValue r = kernel::ctape_combine_values(a, b, op, blend, k, rounding);
        out.d[s] = r.d;
        out.rgb[s * 3] = r.color.x;
        out.rgb[s * 3 + 1] = r.color.y;
        out.rgb[s * 3 + 2] = r.color.z;
    }
    return out;
}

}  // namespace

TEST_CASE("c abi: below + the top layer's own composition is the whole document") {
    const std::vector<clay_brick_request> reqs = equator_bricks();

    struct Arm {
        const char* name;
        int32_t op, blend;
        float k, rounding;
    };
    for (Arm arm : {Arm{"hard add", CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f},
                    Arm{"hard subtract", CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f},
                    Arm{"smooth subtract", CLAY_OP_SUBTRACT, CLAY_BLEND_QUADRATIC, 0.25f, 0.0f},
                    Arm{"smooth add", CLAY_OP_ADD, CLAY_BLEND_CUBIC, 0.3f, 0.0f},
                    Arm{"rounded add", CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.1f},
                    Arm{"intersect", CLAY_OP_INTERSECT, CLAY_BLEND_HARD, 0.0f, 0.0f}}) {
        CAPTURE(arm.name);
        BelowDoc doc(arm.op, arm.blend, arm.k, arm.rounding);

        // The composition is read back through the ABI, not remembered from the
        // setter: what a host has to fold with is what this reports.
        int32_t op = -1, blend = -1;
        float k = -1.0f, rounding = -1.0f;
        REQUIRE(clay_document_layer_composition(doc.d, doc.top, &op, &blend, &k, &rounding) ==
                CLAY_OK);

        const Field whole = whole_field(doc.d, reqs);
        const Field below = below_field(doc.d, doc.top, reqs);
        const Field only = layer_field(doc.d, doc.top, reqs);
        const Field rejoined = host_fold(below, only, op, blend, k, rounding);

        bool near_surface = false;
        for (float v : whole.d) near_surface = near_surface || std::fabs(v) < 0.5f;
        REQUIRE(near_surface);  // or every comparison is two readings of "far outside"

        // BIT-IDENTICAL, in distance and in colour.
        CHECK(differing(rejoined.d, whole.d) == 0);
        CHECK(differing(rejoined.rgb, whole.rgb) == 0);

        // The teeth, without which the equality above could hold for a document
        // whose top layer reached none of these samples.
        CHECK(differing(below.d, whole.d) > 0);
    }

    SUBCASE("and rejoining with a min instead is a different field") {
        // Which is the whole point: the min is what a host composed the
        // EXCLUDING form with, and it is the fold only while the layer unions.
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        const Field whole = whole_field(doc.d, reqs);
        const Field below = below_field(doc.d, doc.top, reqs);
        const Field only = layer_field(doc.d, doc.top, reqs);
        const Field wrong = host_fold(below, only, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f);
        CHECK(differing(wrong.d, whole.d) > 0);
    }

    SUBCASE("the lower layers' own compositions are in the below half, not lost") {
        // The narrow refusal's whole claim: the layers beneath may compose
        // however they like. Hiding the SUBTRACTING cutter changes the below
        // half, so that half is folding it rather than unioning it.
        BelowDoc doc(CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f);
        const Field with_cutter = below_field(doc.d, doc.top, reqs);
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.cutter, 0) == CLAY_OK);
        const Field without = below_field(doc.d, doc.top, reqs);
        CHECK(differing(with_cutter.d, without.d) > 0);
    }

    SUBCASE("it stores no seed, so the whole-document refill after it is unchanged") {
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        const std::uint64_t before = resumed_bricks(doc.d);
        below_field(doc.d, doc.top, reqs);
        CHECK(resumed_bricks(doc.d) == before);

        BelowDoc fresh(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        CHECK(differing(whole_field(doc.d, reqs).d, whole_field(fresh.d, reqs).d) == 0);
    }
}

TEST_CASE("c abi: below refuses only a layer that is not the last visible SDF one") {
    const std::vector<clay_brick_request> reqs = equator_bricks();
    Field out = make_field(reqs.size());
    auto below = [&](clay_document* d, clay_layer_id layer, clay_layer_id* blocking,
                     std::uint32_t* blocking_count = nullptr) {
        return clay_brick_cache_eval_requests_below(d, layer, nullptr, reqs.data(), reqs.size(),
                                                    out.d.data(), out.d.size(), nullptr, 0,
                                                    blocking, blocking_count);
    };

    SUBCASE("a visible SDF layer above blocks it, and the refusal hands back that layer") {
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id detail = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "detail", &detail) == CLAY_OK);
        add_sphere(doc.d, detail, 0.2f, 0.2f);

        clay_layer_id blocking = 987654;
        CHECK(below(doc.d, doc.top, &blocking) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(blocking == detail);
        const std::string why = clay_last_error() ? clay_last_error() : "";
        CHECK(why.find(std::to_string(detail)) != std::string::npos);

        // And the layer that IS the last one still answers, in the same
        // document -- the refusal is about the position, not about the fold.
        blocking = 987654;
        CHECK(below(doc.d, detail, &blocking) == CLAY_OK);
        CHECK(blocking == 0);
    }

    SUBCASE("and it is the LOWEST layer above, which is the one a host can act on") {
        BelowDoc doc(CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id pores = 0, detail = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "pores", &pores) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(doc.d, "detail", &detail) == CLAY_OK);
        add_sphere(doc.d, pores, 0.2f, 0.2f);
        add_sphere(doc.d, detail, 0.2f, 0.3f);
        clay_layer_id blocking = 0;
        std::uint32_t blocking_count = 0;
        CHECK(below(doc.d, doc.form, &blocking, &blocking_count) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(blocking == doc.top);  // not `pores`, not `detail`
        // AND HOW MANY FOLLOW (design.md §12d). The id alone produces "hide or
        // move top", the sculptor does it, and is refused again naming `pores`
        // -- three times over on this stack. The count is what lets the host
        // say so the first time, and it is a different number from the id
        // rather than a restatement of it: three layers are above `form`.
        CHECK(blocking_count == 3);
        const std::string why = clay_last_error() ? clay_last_error() : "";
        CHECK(why.find("3 visible SDF layers are above it in all") != std::string::npos);

        // Hiding the one it named moves the answer DOWN the stack rather than
        // clearing it, which is the behaviour the count is warning about.
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.top, 0) == CLAY_OK);
        blocking = 0;
        blocking_count = 0;
        CHECK(below(doc.d, doc.form, &blocking, &blocking_count) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(blocking == pores);
        CHECK(blocking_count == 2);  // a hidden layer is not in the fold and is not counted
    }

    SUBCASE("one layer above is one, and the message does not say how many") {
        // The singular case has no count clause, so a host echoing the message
        // does not read "1 visible SDF layers".
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id blocking = 0;
        std::uint32_t blocking_count = 0;
        CHECK(below(doc.d, doc.form, &blocking, &blocking_count) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(blocking == doc.top);
        CHECK(blocking_count == 1);
        const std::string why = clay_last_error() ? clay_last_error() : "";
        CHECK(why.find("in all") == std::string::npos);
    }

    SUBCASE("a HIDDEN SDF layer above does not block it") {
        // The stack the artist sees is not the stack that folds. A hidden row
        // above the one being smoothed is not in the whole-document walk, so
        // the split beneath it is still the whole document.
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id detail = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "detail", &detail) == CLAY_OK);
        add_sphere(doc.d, detail, 0.2f, 0.2f);
        REQUIRE(clay_document_set_layer_visible(doc.d, detail, 0) == CLAY_OK);
        clay_layer_id blocking = 987654;
        CHECK(below(doc.d, doc.top, &blocking) == CLAY_OK);
        CHECK(blocking == 0);
    }

    SUBCASE("a voxel layer above does not block it either") {
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id grid_layer = 0;
        clay_voxel_grid* grid = nullptr;
        REQUIRE(clay_document_add_voxel_layer(doc.d, "rasterised", 0.05f, &grid_layer, &grid) ==
                CLAY_OK);
        clay_layer_id blocking = 987654;
        CHECK(below(doc.d, doc.top, &blocking) == CLAY_OK);
        CHECK(blocking == 0);
    }

    SUBCASE("a layer with nothing beneath it answers the far field rather than refusing") {
        // The documented trap: a host that composes unconditionally would
        // subtract its own preview from nothing. The values say so plainly.
        clay_document* one = clay_document_create();
        REQUIRE(one != nullptr);
        clay_layer_id only = 0;
        REQUIRE(clay_add_sdf_layer(one, "only", &only) == CLAY_OK);
        add_sphere(one, only, 1.0f, 0.0f);
        clay_layer_id blocking = 987654;
        CHECK(below(one, only, &blocking) == CLAY_OK);
        CHECK(blocking == 0);
        bool all_far = true;
        for (float v : out.d) all_far = all_far && v > 1e6f;
        CHECK(all_far);
        clay_document_destroy(one);
    }

    SUBCASE("the refusals that are about the layer you named carry no id") {
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id blocking = 987654;
        CHECK(below(doc.d, 4242, &blocking) == CLAY_ERROR_NOT_FOUND);
        CHECK(blocking == 0);

        clay_layer_id grid_layer = 0;
        clay_voxel_grid* grid = nullptr;
        REQUIRE(clay_document_add_voxel_layer(doc.d, "rasterised", 0.05f, &grid_layer, &grid) ==
                CLAY_OK);
        blocking = 987654;
        CHECK(below(doc.d, grid_layer, &blocking) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(blocking == 0);

        blocking = 987654;
        CHECK(below(nullptr, doc.top, &blocking) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(blocking == 0);
    }

    SUBCASE("a NULL out pointer is allowed, and the refusal still refuses") {
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id detail = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "detail", &detail) == CLAY_OK);
        CHECK(below(doc.d, doc.top, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(below(doc.d, detail, nullptr) == CLAY_OK);
    }
}

// -- 12b: the rule that a refusal knowing an id returns it, made executable ---

namespace {

// The blocking id a refusal spells in its MESSAGE. Two of the three refusals
// below have an out-parameter for it; the composition setter has none and
// cannot grow one without changing a signature its callers already hold, so its
// channel is the text -- which is a channel only if something reads it, which
// is what this does.
clay_layer_id layer_named_in_last_error() {
    const char* msg = clay_last_error();
    if (!msg) return 0;
    const std::string s = msg;
    const std::string key = "layer ";
    std::size_t at = s.find(key);
    while (at != std::string::npos) {
        std::size_t i = at + key.size();
        clay_layer_id id = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            id = id * 10 + static_cast<clay_layer_id>(s[i] - '0');
            any = true;
            ++i;
        }
        if (any) return id;
        at = s.find(key, at + key.size());
    }
    return 0;
}

}  // namespace

TEST_CASE("every refusal in this change that knows an id hands it back") {
    // THE RULE, EXECUTABLE (design.md 12b). A refusal that has already computed
    // which layer is responsible and reports only "no" makes the host walk the
    // stack to re-derive it, and turns "hide or move Poros to smooth this layer
    // live" into "not available here". This case fails the day a fourth refusal
    // is added without its id, which is the whole reason it is a test and not a
    // review note.
    const std::vector<clay_brick_request> reqs = equator_bricks();

    SUBCASE("the composition setter on a non-SDF layer names that layer") {
        clay_document* d = clay_document_create();
        REQUIRE(d != nullptr);
        clay_layer_id grid_layer = 0;
        clay_voxel_grid* grid = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d, "rasterised", 0.05f, &grid_layer, &grid) ==
                CLAY_OK);
        REQUIRE(grid_layer != 0);
        CHECK(clay_document_set_layer_composition(d, grid_layer, CLAY_OP_SUBTRACT, CLAY_BLEND_HARD,
                                                  0.0f, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        const clay_layer_id blamed = layer_named_in_last_error();
        CHECK(blamed != 0);
        CHECK(blamed == grid_layer);
        clay_document_destroy(d);
    }

    SUBCASE("writing at an older minor names the layer whose composition it cannot say") {
        BelowDoc doc(CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f);
        clay_layer_id blocking = 0;
        CHECK(clay_document_writable_at_minor(doc.d, 17, &blocking) == CLAY_ERROR_UNSUPPORTED);
        CHECK(blocking != 0);
        CHECK(blocking == doc.cutter);  // the FIRST layer that blocks it, in stack order
    }

    SUBCASE("below on a layer that is not the topmost names the layer above it") {
        BelowDoc doc(CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f);
        Field out = make_field(reqs.size());
        clay_layer_id blocking = 0;
        CHECK(clay_brick_cache_eval_requests_below(doc.d, doc.form, nullptr, reqs.data(),
                                                   reqs.size(), out.d.data(), out.d.size(),
                                                   nullptr, 0, &blocking, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(blocking != 0);
        CHECK(blocking == doc.top);
    }
}
