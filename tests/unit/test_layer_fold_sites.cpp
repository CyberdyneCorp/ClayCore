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
        CHECK(document_fold_is_hard_union(doc));
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
        CHECK_FALSE(document_fold_is_hard_union(doc));
        CHECK(first_composed_fold_layer(doc) == b);
    }

    SUBCASE("a composed layer at the TOP loses both") {
        doc.find_layer(c)->composition = composed(Op::Subtract);
        CHECK_FALSE(layer_join_is_hard_union(doc));
        CHECK_FALSE(document_fold_is_hard_union(doc));
        CHECK(first_composed_fold_layer(doc) == c);
    }

    SUBCASE("the FIRST visible layer's composition is not applied, so it breaks nothing") {
        doc.find_layer(a)->composition = composed(Op::Intersect, BlendProfile::Cubic, 0.4f);
        CHECK(layer_join_is_hard_union(doc));
        CHECK(document_fold_is_hard_union(doc));
        CHECK(first_composed_fold_layer(doc) == 0);
    }

    SUBCASE("a hidden layer is not in the fold, above or below") {
        doc.find_layer(a)->composition = composed(Op::Subtract);
        doc.find_layer(a)->visible = false;
        doc.find_layer(b)->composition = composed(Op::Subtract);
        // `b` is now the first VISIBLE layer, so its operator is not applied.
        CHECK(document_fold_is_hard_union(doc));
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
        CHECK(document_fold_is_hard_union(one));
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
        Document hard = doc;
        hard.layers.back().composition = LayerComposition{};
        CHECK(cull_pad(*hard.layers.back().sdf, hard.layers.back()) ==
              cull_pad(*hard.layers.front().sdf, hard.layers.front()));
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

void add_sphere_to(AbiDoc& doc, clay_layer_id layer, float r, float x) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc.d, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
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
