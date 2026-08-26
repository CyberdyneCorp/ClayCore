#include <doctest/doctest.h>

#include "clay/field/volume.h"
#include <algorithm>

#include "clay/scene/bounds.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// The cull index and coarse plan (scene/cull_index.h) are pure
// accelerations: a per-brick culled compile through them must make exactly
// the cull decisions the plain compile makes. These tests hold the strictest
// form of that — TAPE BYTE-IDENTITY against the unindexed compile — plus the
// contract the cull itself states, band-clamped bit-identity against the
// whole-document tape, over an adversarial corpus: mirrored layers (the
// reflection widens the geometry bound), groups (recursive bounds, blend
// dilation), feathered volume replaces (the compiler's mode choice reads
// what the cull dropped), deformer chains (bounds widen), spline strokes
// (bounds come from tessellation), and layers whose items are all non-local.

using namespace clay;
using namespace clay::kernel;
using namespace clay::scene;

using clay_test::gnarly_document;
using clay_test::item;

namespace {

void require_same_tape(const Tape& a, const Tape& b) {
    REQUIRE(a.instrs.size() == b.instrs.size());
    for (std::size_t i = 0; i < a.instrs.size(); ++i) {
        REQUIRE(a.instrs[i].op == b.instrs[i].op);
        REQUIRE(a.instrs[i].param_offset == b.instrs[i].param_offset);
    }
    REQUIRE(a.params == b.params);  // exact float equality
    REQUIRE(a.blob == b.blob);
    CHECK(a.info.is_exact == b.info.is_exact);
    CHECK(a.info.lipschitz == b.info.lipschitz);
    CHECK(a.bounds.min.x == b.bounds.min.x);
    CHECK(a.bounds.min.y == b.bounds.min.y);
    CHECK(a.bounds.min.z == b.bounds.min.z);
    CHECK(a.bounds.max.x == b.bounds.max.x);
    CHECK(a.bounds.max.y == b.bounds.max.y);
    CHECK(a.bounds.max.z == b.bounds.max.z);
}

// A batch of random brick regions: per brick, the indexed + planned compile
// must be byte-identical to the plain culled compile, and band-clamp
// identical to the full tape inside the brick.
void check_document(const Document& doc, std::uint64_t seed, float lo = -4.0f,
                    float hi = 4.0f) {
    const float band = 0.15f;
    Tape full = compile_document(doc);
    CullIndex index(doc);
    clay_test::Lcg rng(seed);

    std::vector<math::Aabb> bricks;
    math::Aabb batch;
    for (int b = 0; b < 24; ++b) {
        cfloat3 corner = rng.vec3(lo, hi);
        bricks.push_back(math::Aabb{corner, corner + cf3(0.4f, 0.4f, 0.4f)});
        batch.expand(bricks.back().dilated(band));
    }
    CullPlan plan = index.plan(batch);

    for (const math::Aabb& brick : bricks) {
        CullRegion cull{brick.dilated(band)};
        Tape plain = compile_document(doc, &cull);
        Tape indexed = compile_document(doc, &cull, &index, &plan);
        require_same_tape(plain, indexed);
        for (int i = 0; i < 100; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            float df = cclamp(full.eval(p).d, -band, band);
            float dc = cclamp(indexed.eval(p).d, -band, band);
            CHECK(df == dc);  // exact equality, not approx
        }
    }
}

field::FieldVolume feathered_ball(cfloat3 center, float radius) {
    auto ball = [&](cfloat3 p) { return clength(p - center) - radius; };
    field::FieldVolume v = field::FieldVolume::sample(
        ball, math::Aabb{center - cf3(radius + 0.2f, radius + 0.2f, radius + 0.2f),
                         center + cf3(radius + 0.2f, radius + 0.2f, radius + 0.2f)},
        0.04f, 0.12f);
    v.set_feather(0.08f);
    return v;
}

}  // namespace

TEST_CASE("cull index: survivor entries carry the exact direct bounds") {
    Document doc = gnarly_document();
    CullIndex index(doc);
    // Everything survives a plan over an all-containing region, so the plan
    // exposes every prunable chain's cached entries; each must be bit-equal
    // to the bound the compiler would compute for that node itself.
    CullPlan plan = index.plan(math::Aabb{cf3(-100, -100, -100), cf3(100, 100, 100)});
    int checked = 0;
    for (const Layer& layer : doc.layers) {
        if (!layer.visible || layer.kind != LayerKind::Sdf || !layer.sdf) continue;
        const std::vector<CullIndex::Entry>* entries = plan.chain(layer, layer.sdf->roots);
        REQUIRE(entries);
        for (const CullIndex::Entry& e : *entries) {
            const Node* n = layer.sdf->find(e.id);
            REQUIRE(n);
            REQUIRE(e.node == n);
            math::Aabb direct = n->is_group ? node_influence_bound(*layer.sdf, e.id, layer)
                                            : item_geometry_bound(*n, layer);
            CHECK(e.bound.min.x == direct.min.x);
            CHECK(e.bound.max.x == direct.max.x);
            CHECK(e.bound.min.y == direct.min.y);
            CHECK(e.bound.max.y == direct.max.y);
            CHECK(e.bound.min.z == direct.min.z);
            CHECK(e.bound.max.z == direct.max.z);
            if (!n->is_group) CHECK(e.local == item_influence_is_local(*n));
            ++checked;
        }
    }
    CHECK(checked > 5);
    // The cached pad is what the compiler would recompute per compile: the
    // feather term (zero here, no feathered replace in this corpus) plus the
    // blend term, which is NOT zero -- the corpus blends, and a smooth-union
    // chain can be steered by an item outside its own bound.
    float expected = 0.0f;
    for (const Layer& layer : doc.layers)
        if (layer.visible && layer.kind == LayerKind::Sdf && layer.sdf)
            expected = std::max(expected, cull_pad(*layer.sdf, layer));
    CHECK(expected > 0.0f);
    CHECK(index.cull_pad() == expected);
}

TEST_CASE("cull index: byte-identical per-brick tapes on the gnarly corpus") {
    // Mirror + mirrored items, nested groups 4 deep with blends, a stroke,
    // paint, layer transforms and an instanced layer (shared SdfContent
    // compiled under two layers — the case that keys the index per layer).
    check_document(gnarly_document(), 401);
}

TEST_CASE("cull index: a long smooth-union chain is culled without moving the field") {
    // The case the corpus above does not reach. A blend's own influence bound
    // covers what ONE blend can move; a CHAIN is different, because the
    // accumulated value part way down it sits well above where it ends up, so
    // an item whose final contribution is nothing can still be within k of the
    // RUNNING value and steer it.
    //
    // It needs length to show. Measured at band-only dilation on this shape:
    // 5 items agree exactly, 25 differ by up to 0.009 — half a cell at the
    // resolution this bakes at — and 600 differ in 95 samples of 1627. The
    // magnitude falls as the chain grows and the COUNT rises, which is what a
    // crowd of items each contributing near its own edge looks like.
    Document doc;
    Layer& l = doc.add_sdf_layer("chain");
    Node base;
    base.prim = Prim::sphere(1.0f);
    l.sdf->insert(base);
    for (int i = 1; i < 200; ++i) {
        Node dab;
        dab.prim = Prim::sphere(0.05f);
        dab.blend = Blend{BlendProfile::Quadratic, 0.08f};
        const float a = 0.3f * std::sin(static_cast<float>(i) * 0.7f);
        const float b = 0.3f * std::cos(static_cast<float>(i) * 1.3f);
        dab.xform.position =
            cf3(-std::sqrt(std::max(0.0f, 1.0f - a * a - b * b)), a, b);
        l.sdf->insert(dab);
    }
    // Tight bounds: the shape is a unit sphere, so bricks scattered over
    // [-4, 4] would mostly miss it and the samples that matter are the ones
    // near the surface.
    check_document(doc, 9107, -1.4f, 1.4f);
}

TEST_CASE("cull index: mirrored layer with spline strokes and deformer chains") {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.mirror_axes = kMirrorX;
    l.mirror_k = 0.06f;

    // A mirrored item far on +x: its geometry bound must include the -x
    // reflection or coarse pruning would drop it from bricks near -x.
    Node ear = item(Prim::sphere(0.4f), cf3(2.5f, 0.3f, 0));
    ear.mirror = true;
    l.sdf->insert(ear);

    // Catmull-Rom spline stroke: bounds come from the tessellated curve.
    Node stroke;
    stroke.prim = Prim::stroke();
    stroke.stroke = {{cf3(-2, 0, 0), 0.3f, StrokePointType::Spline},
                     {cf3(-1, 1.2f, 0.4f), 0.25f, StrokePointType::Spline},
                     {cf3(0, -0.5f, 0.8f), 0.2f, StrokePointType::Spline},
                     {cf3(1.5f, 0.4f, -0.6f), 0.3f, StrokePointType::Spline}};
    stroke.stroke_blend_k = 0.05f;
    stroke.blend = Blend{BlendProfile::Quadratic, 0.08f};
    l.sdf->insert(stroke);

    // Deformer chains that widen bounds: twist + noise on a box, a big bend
    // on a cylinder.
    Node twisted = item(Prim::box(cf3(0.5f, 1.0f, 0.5f)), cf3(0, -2, 0));
    Deformer twist;
    twist.type = kernel::cdeform_twist;
    twist.k = 2.0f;
    Deformer noise;
    noise.type = kernel::cdeform_noise;
    noise.k = 0.15f;
    noise.a = 2.0f;
    noise.b = 3.0f;
    noise.c = 0.5f;
    twisted.deformers = {twist, noise};
    twisted.mirror = true;
    l.sdf->insert(twisted);

    Node bent = item(Prim::capped_cylinder(0.3f, 1.2f), cf3(2, 2, 0), Op::Subtract,
                     Blend{BlendProfile::Cubic, 0.05f});
    Deformer bend;
    bend.type = kernel::cdeform_bend;
    bend.k = 1.2f;
    bent.deformers = {bend};
    l.sdf->insert(bent);

    check_document(doc, 402);
}

TEST_CASE("cull index: groups, blend dilation, repeats and non-local layers") {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    // Group with a wide blend: the group bound dilates by its support, so a
    // brick just outside the children still keeps the group.
    Node g;
    g.is_group = true;
    g.op = Op::Add;
    g.blend = Blend{BlendProfile::Quadratic, 0.4f};
    NodeId gid = l.sdf->insert(g);
    l.sdf->insert(item(Prim::sphere(0.5f), cf3(-2, 0, 0)), gid);
    l.sdf->insert(item(Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(-2.5f, 0.5f, 0),
                       Op::Subtract, Blend{BlendProfile::Quadratic, 0.1f}),
                  gid);
    // Inline group whose children continue the outer chain.
    Node inl;
    inl.is_group = true;
    inl.op = Op::None;
    NodeId iid = l.sdf->insert(inl);
    l.sdf->insert(item(Prim::torus(0.6f, 0.12f), cf3(2, -1, 0)), iid);

    // Radial and finite-grid repeats (finite bounds sweep every copy), and
    // an infinite grid (never culled).
    Node radial = item(Prim::sphere(0.15f), cf3(0, 2, 0));
    radial.repeat = Repeat::radial(6, 0.8f);
    l.sdf->insert(radial);
    Node grid = item(Prim::sphere(0.1f), cf3(0, -2.5f, 0));
    grid.repeat = Repeat::grid_finite(0.6f, cf3(2, 0, 2));
    l.sdf->insert(grid);
    Node inf = item(Prim::sphere(0.05f), cf3(0, 0, -3), Op::Subtract);
    inf.repeat = Repeat::grid_infinite(cf3(2, 2, 2));
    l.sdf->insert(inf);

    // A layer whose items are all non-local: an unbounded plane and an
    // intersect — nothing here may ever be culled.
    Layer& nl = doc.add_sdf_layer("nonlocal");
    nl.sdf->insert(item(Prim::sphere(3.0f), cf3(0, 0, 0)));
    nl.sdf->insert(item(Prim::sphere(2.8f), cf3(0.5f, 0, 0), Op::Intersect));

    check_document(doc, 403);
}

TEST_CASE("cull index: feathered replace chains keep the full walk") {
    // The compiler's choice between the feathered and the hard replace reads
    // whether the cull dropped anything from the chain BEFORE the volume. A
    // coarse prune that hid a dropped item would flip that choice, so chains
    // holding a feathered replace are never pruned. Two documents pin both
    // sides of the choice:
    //
    // (a) carve-first: [Subtract far, feathered volume] — with nothing
    // beneath it the subtract is SKIPPED (not cull-dropped) and the volume
    // takes the hard replace. Naive pruning would have counted the subtract
    // as dropped and chosen the feathered mode.
    Document a;
    Layer& la = a.add_sdf_layer("l");
    la.sdf->insert(item(Prim::sphere(0.5f), cf3(6, 0, 0), Op::Subtract));
    Node va = item(Prim::volume(), cf3(0, 0, 0), Op::Replace);
    va.volume = std::make_shared<field::FieldVolume>(feathered_ball(cf3(0, 0, 0), 0.8f));
    la.sdf->insert(va);
    check_document(a, 404, -1.5f, 1.5f);

    // (b) add-first: [Add far, feathered volume] — the far add IS
    // cull-dropped for bricks near the volume, so the feathered mode is
    // chosen even with no accumulator. Pruning that lost the drop would
    // have degraded it to the hard replace.
    Document b;
    Layer& lb = b.add_sdf_layer("l");
    lb.sdf->insert(item(Prim::sphere(0.5f), cf3(6, 0, 0)));
    Node vb = item(Prim::volume(), cf3(0, 0, 0), Op::Replace);
    vb.volume = std::make_shared<field::FieldVolume>(feathered_ball(cf3(0, 0, 0), 0.8f));
    lb.sdf->insert(vb);
    check_document(b, 405, -1.5f, 1.5f);

    // The pad the index caches equals the walk it replaced.
    CullIndex index(b);
    CHECK(index.cull_pad() == feather_cull_pad(*lb.sdf, lb));
    CHECK(index.cull_pad() > 0.0f);

    // (c) a feathered replace nested in a group, with prunable siblings
    // outside the group: only the volume's own chain refuses pruning.
    Document c;
    Layer& lc = c.add_sdf_layer("l");
    lc.sdf->insert(item(Prim::sphere(0.6f), cf3(0, 0, 0)));
    lc.sdf->insert(item(Prim::sphere(0.3f), cf3(5, 0, 0)));
    Node g;
    g.is_group = true;
    g.op = Op::Add;
    NodeId gid = lc.sdf->insert(g);
    lc.sdf->insert(item(Prim::sphere(0.4f), cf3(4.5f, 1, 0), Op::Subtract), gid);
    Node vc = item(Prim::volume(), cf3(0.5f, 0, 0), Op::Replace);
    vc.volume = std::make_shared<field::FieldVolume>(feathered_ball(cf3(0.5f, 0, 0), 0.5f));
    lc.sdf->insert(vc, gid);
    check_document(c, 406, -2.0f, 2.0f);
}

TEST_CASE("cull plan: a coarse region actually prunes the far document") {
    // Not a timing assertion — the scaling gate lives in benchmarks — but
    // the mechanism must observably shorten a chain, or every "speedup"
    // above is vacuous.
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    for (int i = 0; i < 64; ++i)
        l.sdf->insert(item(Prim::sphere(0.05f),
                           cf3(3.0f + 0.2f * static_cast<float>(i), 0, 0)));
    NodeId near_id = l.sdf->insert(item(Prim::sphere(0.5f), cf3(0, 0, 0)));

    CullIndex index(doc);
    math::Aabb region{cf3(-0.7f, -0.7f, -0.7f), cf3(0.7f, 0.7f, 0.7f)};
    CullPlan plan = index.plan(region);
    const std::vector<CullIndex::Entry>* pruned = plan.chain(l, l.sdf->roots);
    REQUIRE(pruned);
    REQUIRE(pruned->size() == 1);
    CHECK((*pruned)[0].id == near_id);

    // ...and an index built for one document is refused for another rather
    // than trusted: the compile falls back to computing bounds itself.
    Document other;
    Layer& lo = other.add_sdf_layer("l");
    lo.sdf->insert(item(Prim::sphere(0.5f), cf3(0, 0, 0)));
    CullRegion cull{region};
    Tape plain = compile_document(other, &cull);
    Tape guarded = compile_document(other, &cull, &index, &plan);
    require_same_tape(plain, guarded);
}

// -- the spatial index (add-item-spatial-index) ------------------------------
//
// CullIndex used to answer a plan by asking EVERY entry whether it intersects.
// That is a linear scan however cheap each step is, measured at a flat ~2.8 ns
// per item across a 300x range in document size. The proposal's own task 1.10
// names the failure mode this must avoid: "a 2x constant improvement passing as
// a fix for this is the failure mode" — so what matters is the SHAPE.

namespace {

// The survivors, computed straight from the DOCUMENT by the same definition
// the compiler uses. Deliberately not from CullIndex's own entries: comparing
// the tree against a scan of the tree's own inputs would pass even if the
// inputs were wrong, and `item_geometry_bound` / `item_influence_is_local` are
// the single definition of whether an item may be culled (scene-model).
std::vector<NodeId> expected_survivors(const Layer& l,
                                       const math::Aabb& test) {
    std::vector<NodeId> kept;
    for (NodeId id : l.sdf->roots) {
        const Node* n = l.sdf->find(id);
        if (!n || !n->visible) continue;
        const math::Aabb bound = item_geometry_bound(*n, l);
        const bool local = item_influence_is_local(*n);
        if (!local || bound.is_infinite() || bound.intersects(test)) kept.push_back(id);
    }
    return kept;
}

Document scattered(int count, int stride = 100) {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    for (int i = 0; i < count; ++i) {
        Node n;
        n.prim = Prim::sphere(0.05f);
        n.xform.position = cf3(static_cast<float>(i % stride) * 0.1f,
                               static_cast<float>((i / stride) % stride) * 0.1f,
                               static_cast<float>(i / (stride * stride)) * 0.1f);
        l.sdf->insert(n);
    }
    return doc;
}

}  // namespace

TEST_CASE("cull index: the tree returns exactly what the scan did, in the same order") {
    // The equivalence that makes this a speedup rather than a behaviour change.
    // Chain ORDER matters — the compiler applies items in it — so this checks
    // the sequence, not the set.
    Document doc = scattered(500);
    const Layer& l = doc.layers[0];
    CullIndex index(doc);

    const math::Aabb probes[] = {
        math::Aabb{cf3(0, 0, 0), cf3(0.3f, 0.3f, 0.3f)},        // a corner
        math::Aabb{cf3(2.0f, 2.0f, 0), cf3(2.4f, 2.4f, 0.2f)},  // the middle
        math::Aabb{cf3(50, 50, 50), cf3(51, 51, 51)},           // nothing at all
        math::Aabb{cf3(-5, -5, -5), cf3(20, 20, 20)},           // everything
    };
    for (const math::Aabb& region : probes) {
        CAPTURE(region.min.x);
        CullPlan plan = index.plan(region);
        const std::vector<CullIndex::Entry>* got = plan.chain(l, l.sdf->roots);
        REQUIRE(got);

        // The index dilates by the feather pad exactly as the plan does.
        const std::vector<NodeId> want = expected_survivors(
            l, index.cull_pad() > 0.0f ? region.dilated(index.cull_pad()) : region);
        REQUIRE(got->size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) CHECK((*got)[i].id == want[i]);
    }
}

TEST_CASE("cull index: an entry that can never be culled always survives") {
    // Non-local items and infinite bounds are held OUT of the tree — they
    // survive every query by definition, so a node containing one would be
    // visited every time and its subtree would be pure overhead. Which means
    // the merge that puts them back has to be right.
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    // A plane is unbounded: it reaches everywhere and can never be culled.
    const NodeId plane = l.sdf->insert(item(Prim::plane(cf3(0, 1, 0), 0.0f), cf3(0, 0, 0)));
    for (int i = 0; i < 40; ++i)
        l.sdf->insert(item(Prim::sphere(0.05f), cf3(10.0f + 0.2f * static_cast<float>(i), 0, 0)));
    const NodeId near = l.sdf->insert(item(Prim::sphere(0.3f), cf3(0, 0, 0)));

    CullIndex index(doc);
    CullPlan plan = index.plan(math::Aabb{cf3(-0.5f, -0.5f, -0.5f), cf3(0.5f, 0.5f, 0.5f)});
    const std::vector<CullIndex::Entry>* got = plan.chain(l, l.sdf->roots);
    REQUIRE(got);
    REQUIRE(got->size() == 2);
    // And in CHAIN order: the plane was inserted first.
    CHECK((*got)[0].id == plane);
    CHECK((*got)[1].id == near);
}

TEST_CASE("cull index: the cost is SUBLINEAR in document size, not merely smaller") {
    // The gate the proposal asked for. A constant-factor win would keep the
    // per-item cost flat while lowering it; this asserts the per-item cost
    // FALLS as the document grows, which only a search can do.
    //
    // Not a wall-clock budget — those live in benchmarks and on the device —
    // but a shape assertion, which is what "a 2x constant is the failure mode"
    // actually demands.
    // On the y = 0 row, which is populated at BOTH sizes. A region at y = 1.0
    // needs 1000 items before anything is near it, so the first draft of this
    // measured zero survivors at 500 and compared against nothing.
    const math::Aabb region{cf3(1.0f, 0.0f, 0.0f), cf3(1.08f, 0.08f, 0.08f)};

    const auto survivors_at = [&](int items) {
        Document doc = scattered(items);
        const Layer& l = doc.layers[0];
        CullIndex index(doc);
        CullPlan plan = index.plan(region);
        const std::vector<CullIndex::Entry>* got = plan.chain(l, l.sdf->roots);
        return got ? got->size() : 0;
    };

    // The premise the whole design rests on, and the one thing worth asserting
    // without a clock: a fixed region keeps a FLAT number of survivors however
    // large the document gets. If that ever stopped holding, no index could
    // help and the proposal's reasoning would be void.
    const std::size_t small = survivors_at(500);
    const std::size_t large = survivors_at(20000);
    CHECK(small > 0);
    CHECK(large > 0);
    CHECK(large <= small * 2);  // flat, not growing with the document
}

namespace {

// The property an appended index must have: for every brick of a batch, the
// tape it produces is byte-identical to the one a FRESHLY BUILT index
// produces — which the tests above already hold equal to the unindexed
// compile, so this inherits that too.
void check_append_matches_fresh(Document& doc, const std::vector<NodeId>& appended,
                                CullIndex& grown, std::uint64_t seed) {
    const float band = 0.15f;
    REQUIRE(grown.append(appended));

    const CullIndex fresh(doc);
    CHECK(grown.cull_pad() == fresh.cull_pad());  // exact, not approx

    clay_test::Lcg rng(seed);
    std::vector<math::Aabb> bricks;
    math::Aabb batch;
    for (int b = 0; b < 24; ++b) {
        cfloat3 corner = rng.vec3(-2.0f, 2.0f);
        bricks.push_back(math::Aabb{corner, corner + cf3(0.4f, 0.4f, 0.4f)});
        batch.expand(bricks.back().dilated(band));
    }
    const CullPlan grown_plan = grown.plan(batch);
    const CullPlan fresh_plan = fresh.plan(batch);

    for (const math::Aabb& brick : bricks) {
        CullRegion cull{brick.dilated(band)};
        const Tape a = compile_document(doc, &cull, &grown, &grown_plan);
        const Tape b = compile_document(doc, &cull, &fresh, &fresh_plan);
        require_same_tape(a, b);
        // And against the compile that uses no index at all, which is the
        // claim the whole class rests on.
        require_same_tape(compile_document(doc, &cull), a);
    }
}

Node dab(cfloat3 at, float k = 0.05f) {
    Node n = item(Prim::sphere(0.25f), at);
    n.blend = Blend{BlendProfile::Quadratic, k};
    return n;
}

}  // namespace

TEST_CASE("cull index: an appended index is the index a rebuild would give") {
    SUBCASE("one dab onto the gnarly corpus") {
        Document doc = gnarly_document();
        CullIndex grown(doc);
        const NodeId id = doc.layers[0].sdf->insert(dab(cf3(0.4f, 0.2f, -0.3f)));
        check_append_matches_fresh(doc, {id}, grown, 811);
    }

    SUBCASE("several at once") {
        Document doc = gnarly_document();
        CullIndex grown(doc);
        std::vector<NodeId> ids;
        for (int i = 0; i < 5; ++i)
            ids.push_back(doc.layers[0].sdf->insert(dab(cf3(0.1f * i, 0.3f, 0.2f))));
        check_append_matches_fresh(doc, ids, grown, 812);
    }

    SUBCASE("a GROUP, whose children are chains of their own") {
        Document doc = gnarly_document();
        CullIndex grown(doc);
        Node g;
        g.is_group = true;
        g.blend = Blend{BlendProfile::Quadratic, 0.07f};
        const NodeId gid = doc.layers[0].sdf->insert(g);
        REQUIRE(gid != kNoNode);
        doc.layers[0].sdf->insert(dab(cf3(0.5f, -0.2f, 0.1f)), gid);
        doc.layers[0].sdf->insert(dab(cf3(0.6f, -0.1f, 0.2f)), gid);
        check_append_matches_fresh(doc, {gid}, grown, 813);
    }

    SUBCASE("a feathered volume replace, which forbids pruning its chain") {
        // The entry that changes the chain's PRUNABILITY rather than its
        // bounds: if the append missed it, the grown index would prune a chain
        // the compiler needs the full walk of, and the tapes would differ.
        Document doc = gnarly_document();
        CullIndex grown(doc);
        Node n;
        n.prim = Prim::volume();
        n.volume =
            std::make_shared<field::FieldVolume>(feathered_ball(cf3(0.3f, 0.1f, 0.0f), 0.4f));
        n.op = Op::Replace;
        const NodeId id = doc.layers[0].sdf->insert(std::move(n));
        check_append_matches_fresh(doc, {id}, grown, 814);
    }

    SUBCASE("an item whose blend widens the document's cull pad") {
        // The pad is a maximum over visible nodes, so an append can raise it.
        // A grown index that kept the old pad would plan against too small a
        // region and drop an item a brick needed.
        Document doc = gnarly_document();
        CullIndex grown(doc);
        const float before = grown.cull_pad();
        const NodeId id = doc.layers[0].sdf->insert(dab(cf3(0.2f, 0.2f, 0.2f), 1.5f));
        check_append_matches_fresh(doc, {id}, grown, 815);
        CHECK(grown.cull_pad() > before);  // it really did move
    }

    SUBCASE("a stroke: every dab carried forward on the same index") {
        // The case this exists for. Each append builds on the one before, so a
        // drift would compound rather than show up once.
        Document doc = gnarly_document();
        CullIndex grown(doc);
        for (int i = 0; i < 8; ++i) {
            const NodeId id = doc.layers[0].sdf->insert(
                dab(cf3(0.35f + 0.05f * static_cast<float>(i), 0.1f, -0.2f)));
            REQUIRE(grown.append({id}));
        }
        const CullIndex fresh(doc);
        CHECK(grown.cull_pad() == fresh.cull_pad());
        check_append_matches_fresh(doc, {doc.layers[0].sdf->insert(dab(cf3(0.8f, 0.0f, 0.0f)))},
                                   grown, 816);
    }
}

TEST_CASE("cull index: an append it cannot be sure of is refused, not guessed") {
    Document doc = gnarly_document();
    const std::vector<NodeId>& roots = doc.layers[0].sdf->roots;

    SUBCASE("nothing appended") {
        CullIndex ix(doc);
        CHECK_FALSE(ix.append({}));
    }
    SUBCASE("ids that are not the tail") {
        CullIndex ix(doc);
        CHECK_FALSE(ix.append({roots.front()}));
    }
    SUBCASE("more ids than the layer holds") {
        CullIndex ix(doc);
        std::vector<NodeId> too_many(roots.size() + 2, roots.back());
        CHECK_FALSE(ix.append(too_many));
    }
    SUBCASE("a refusal leaves the index usable") {
        // Nothing is written until every check has passed, so the index a
        // refused caller still holds is the one it had.
        CullIndex ix(doc);
        const float pad = ix.cull_pad();
        CHECK_FALSE(ix.append({roots.front()}));
        CHECK(ix.cull_pad() == pad);
        const CullIndex fresh(doc);
        const math::Aabb region{cf3(-2, -2, -2), cf3(2, 2, 2)};
        CullRegion cull{region};
        const CullPlan kept_plan = ix.plan(region);
        const CullPlan fresh_plan = fresh.plan(region);
        require_same_tape(compile_document(doc, &cull, &ix, &kept_plan),
                          compile_document(doc, &cull, &fresh, &fresh_plan));
    }
}

// -- what an influence bound PROMISES (issue #319) ----------------------------
//
// item_influence_bound answers "the box outside which this item cannot change
// the field", and a host uses it as the box to DIRTY. That promise had never
// been stated as a test: the cull tests above hold the CULL contract, which is
// a different one and reads a different bound (item_geometry_bound for items).
//
// The property here is the promise itself, and it is checked the way a host
// would be hurt if it were false: edit a node, and require every band-clamped
// value OUTSIDE the box to be unchanged. A box that is too small shows up as a
// point that moved and would have been left stale.
//
// The union of the bound BEFORE and AFTER the edit is what a host dirties --
// command_influence_bound says so, and for the same reason: on one side of an
// add the node is not there, and a move has two ends.
namespace {

// Every band-clamped value outside `dirty` must be identical across the edit.
// Returns how many points were actually checked, so a caller can refuse a
// vacuous run rather than pass one.
int require_unchanged_outside(const Tape& before, const Tape& after, const math::Aabb& dirty,
                              float band, std::uint64_t seed, float lo, float hi, float* worst,
                              int samples = 4000) {
    clay_test::Lcg rng(seed);
    int checked = 0;
    for (int i = 0; i < samples; ++i) {
        cfloat3 p = rng.vec3(lo, hi);
        if (dirty.contains(p)) continue;  // inside: free to move
        const float a = cclamp(before.eval(p).d, -band, band);
        const float b = cclamp(after.eval(p).d, -band, band);
        const float dv = a > b ? a - b : b - a;
        if (dv > *worst) *worst = dv;
        // EXACT, not approx. Outside the box the item did not participate in
        // any value that survives the clamp, so the two evaluate the same
        // instructions over the same floats. A tolerance here would admit
        // precisely the staleness this exists to catch: the defect measured
        // 0.119 against a 0.15 band, and a "close enough" bound would have
        // passed at anything above that.
        CHECK(a == b);
        ++checked;
    }
    return checked;
}

// Move every visible ITEM of every visible SDF layer in turn, and hold the
// promise for each. `moved` counts the nodes actually exercised.
void check_influence_promise(Document doc, std::uint64_t seed, float lo, float hi,
                             int* out_moved, int* out_points, int* out_infinite,
                             float* out_worst) {
    const float band = 0.15f;
    const cfloat3 delta = cf3(0.11f, -0.07f, 0.05f);
    int moved = 0, points = 0, infinite = 0;
    for (Layer& layer : doc.layers) {
        if (!layer.visible || layer.kind != LayerKind::Sdf || !layer.sdf) continue;
        std::vector<NodeId> ids;
        for (const auto& [id, n] : layer.sdf->nodes()) {
            if (n.is_group || !n.visible) continue;
            ids.push_back(id);
        }
        for (NodeId id : ids) {
            Node* n = layer.sdf->find_mut(id);
            REQUIRE(n);
            const math::Aabb before_box =
                node_influence_bound_in_document(doc, *layer.sdf, id);
            const Tape before = compile_document(doc);
            const cfloat3 was = n->xform.position;
            n->xform.position = was + delta;
            const math::Aabb after_box =
                node_influence_bound_in_document(doc, *layer.sdf, id);
            const Tape after = compile_document(doc);

            if (before_box.is_infinite() || after_box.is_infinite()) {
                // Claims everything: trivially true, and counted so the suite
                // can show the corpus still exercises the finite path.
                ++infinite;
            } else {
                math::Aabb dirty = before_box;
                dirty.expand(after_box);
                // The band dilation every consumer applies: a local item's
                // bound is its geometry bound and relies on the same one.
                // The band, PLUS the chain pad the compiler already applies when
                // culling (scene::cull_pad, #282): an item's own bound is what
                // ONE blend can move, and a smooth-union chain drags further.
                const float pad = band + cull_pad(*layer.sdf, layer);
                points += require_unchanged_outside(before, after, dirty.dilated(pad), band,
                                                    seed + id, lo, hi, out_worst);
                ++moved;
            }
            n->xform.position = was;  // leave the document as it was found
        }
    }
    *out_moved = moved;
    *out_points = points;
    *out_infinite = infinite;
}

}  // namespace

TEST_CASE("influence bound: an edit changes nothing outside the box, on the gnarly corpus") {
    int moved = 0, points = 0, infinite = 0;
    float worst = 0;
    check_influence_promise(gnarly_document(), 8801, -3.0f, 3.0f, &moved, &points, &infinite, &worst);
    MESSAGE("gnarly: worst band-clamped drift outside the dirty box = ", worst);
    // Guards against a vacuous pass: the corpus must actually have exercised
    // the finite path, and points must actually have landed outside the boxes.
    CHECK(moved > 5);
    CHECK(points > 500);
}

TEST_CASE("influence bound: an INTERSECT is bounded, and the box it names holds") {
    // The case #319 is about. An intersect reads the accumulated field far from
    // itself, so its bound is not its own geometry — but it is not Everything
    // either, and this is what decides which.
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    l.sdf->insert(item(Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(-0.4f, 0, 0), Op::Intersect));
    // and something blended AFTER it, so the intersect's value feeds a further
    // combine and can move the result away from the intersect's own geometry —
    // the case a bound of the item's own box would miss.
    l.sdf->insert(item(Prim::sphere(0.35f), cf3(0.7f, 0.2f, 0), Op::Add,
                       Blend{BlendProfile::Quadratic, 0.12f}));

    int moved = 0, points = 0, infinite = 0;
    float worst = 0;
    check_influence_promise(doc, 8802, -2.5f, 2.5f, &moved, &points, &infinite, &worst);
    MESSAGE("intersect: worst drift = ", worst);
    CHECK(moved > 0);
    CHECK(points > 500);
}

TEST_CASE("influence bound: a layer of non-local ops still holds its promise") {
    Document doc;
    Layer& nl = doc.add_sdf_layer("nonlocal");
    nl.sdf->insert(item(Prim::sphere(1.2f), cf3(0, 0, 0)));
    nl.sdf->insert(item(Prim::sphere(2.8f), cf3(0.5f, 0, 0), Op::Intersect));
    nl.sdf->insert(item(Prim::box(cf3(0.6f, 0.6f, 0.6f)), cf3(0, 0.8f, 0), Op::Intersect,
                        Blend{BlendProfile::Quadratic, 0.05f}));
    int moved = 0, points = 0, infinite = 0;
    float worst = 0;
    check_influence_promise(doc, 8803, -3.5f, 3.5f, &moved, &points, &infinite, &worst);
    MESSAGE("non-local: worst drift = ", worst);
    CHECK(moved > 0);
    CHECK(points > 500);
}

TEST_CASE("influence bound: a MIRRORED item's box understates what moving it changes") {
    // Isolated from the gnarly corpus, where every one of the worst drifts came
    // from a node with mirror set. item_geometry_bound does reflect the box and
    // does dilate by the mirror seam's blend support, so the bound is not
    // ignoring the mirror -- it is understating it by enough to matter.
    Document doc;
    Layer& l = doc.add_sdf_layer("body");
    l.mirror_axes = kMirrorX;
    l.mirror_k = 0.08f;
    l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    Node ear = item(Prim::round_cone(0.25f, 0.1f, 0.4f), cf3(0.9f, 0.6f, 0));
    ear.mirror = true;
    ear.blend = Blend{BlendProfile::Quadratic, 0.05f};
    l.sdf->insert(ear);

    int moved = 0, points = 0, infinite = 0;
    float worst = 0;
    check_influence_promise(doc, 8804, -3.0f, 3.0f, &moved, &points, &infinite, &worst);
    MESSAGE("mirrored-only: worst drift = ", worst);
    CHECK(moved > 0);
    CHECK(points > 500);
}

TEST_CASE("influence bound: the same document WITHOUT the mirror") {
    // The control. Same geometry, same blend, mirror off -- if this one is
    // clean the drift above is the mirror rather than the chain.
    Document doc;
    Layer& l = doc.add_sdf_layer("body");
    l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    Node ear = item(Prim::round_cone(0.25f, 0.1f, 0.4f), cf3(0.9f, 0.6f, 0));
    ear.blend = Blend{BlendProfile::Quadratic, 0.05f};
    l.sdf->insert(ear);

    int moved = 0, points = 0, infinite = 0;
    float worst = 0;
    check_influence_promise(doc, 8805, -3.0f, 3.0f, &moved, &points, &infinite, &worst);
    MESSAGE("no-mirror control: worst drift = ", worst);
    CHECK(moved > 0);
    CHECK(points > 500);
}

TEST_CASE("influence bound: an INSTANCED layer's other copy is inside the box") {
    // The defect, isolated. instance_layer copies the Layer and SHARES the
    // SdfContent by shared_ptr, so one node is compiled once per instancing
    // layer under that layer's own transform -- and editing it moves every
    // copy. A bound naming one copy left the others stale: 0.103 outside the
    // box against a band of 0.15, before node_influence_bound_in_document.
    Document doc;
    Layer& body = doc.add_sdf_layer("body");
    body.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    body.sdf->insert(item(Prim::box(cf3(0.4f, 0.4f, 0.4f)), cf3(0.6f, 0, 0), Op::Add,
                          Blend{BlendProfile::Quadratic, 0.1f}));
    Layer* inst = doc.instance_layer(doc.layers[0].id, "body-instance");
    REQUIRE(inst);
    inst->xform.position = cf3(3, 0, 0);

    int moved = 0, points = 0, infinite = 0;
    float worst = 0;
    check_influence_promise(doc, 8806, -5.0f, 5.0f, &moved, &points, &infinite, &worst);
    CHECK(moved > 0);
    CHECK(points > 500);
}

TEST_CASE("influence bound: the same document with the instance removed") {
    // The control: without the second layer the per-layer bound was already
    // right, which is what makes the case above about instancing rather than
    // about the geometry.
    Document doc;
    Layer& body = doc.add_sdf_layer("body");
    body.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    body.sdf->insert(item(Prim::box(cf3(0.4f, 0.4f, 0.4f)), cf3(0.6f, 0, 0), Op::Add,
                          Blend{BlendProfile::Quadratic, 0.1f}));

    int moved = 0, points = 0, infinite = 0;
    float worst = 0;
    check_influence_promise(doc, 8807, -5.0f, 5.0f, &moved, &points, &infinite, &worst);
    CHECK(moved > 0);
    CHECK(points > 500);
}
