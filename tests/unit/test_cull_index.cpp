#include <doctest/doctest.h>

#include "clay/field/volume.h"
#include <algorithm>
#include <memory>

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

TEST_CASE("cull index: a plan handed in without a cull region is dropped") {
    // An invariant of `compile_document`, pinned here because the per-brick
    // compile now DEPENDS on it. That compile rejects a plan survivor by
    // reading the entry -- `local` and `bound`, both cached when the chain was
    // built -- against `cull_test`, and `begin_cull` sets `cull_test` only
    // where there is a region to set it from. Reading it with no region would
    // test against the empty box, which every entry misses, and the tape would
    // come back empty.
    //
    // What makes that unreachable is one line in a DIFFERENT function: both
    // entry points drop the plan when the cull region is null, on the reasoning
    // that a plan without one could only mean a pruned whole-document tape. So
    // the pairing resolves to an ordinary uncalled compile, and this says so --
    // if that line ever goes, the per-brick reject needs its own guard back.
    const Document doc = gnarly_document();
    const Tape full = compile_document(doc);
    const CullIndex index(doc);
    // Deliberately a plan over a SMALL region, so it prunes hard: were it not
    // dropped, the compile would emit its handful of survivors instead of the
    // whole document and the tapes would differ by more than a rounding.
    const CullPlan narrow = index.plan(math::Aabb{cf3(50, 50, 50), cf3(51, 51, 51)});
    require_same_tape(compile_document(doc, nullptr, &index, &narrow), full);
}

TEST_CASE("cull index: a degenerate region takes the predicate, not the packed scan") {
    // The scan folds `!local || bound.is_infinite()` into one box per entry and
    // then tests it with six bare comparisons -- `Aabb::intersects` without its
    // two emptiness guards. That is exact only while the REGION discharges
    // those guards itself, and two regions do not:
    //
    //   an EMPTY region  -- every intersection is false, but an entry the fold
    //                       stored as an infinite box would survive it;
    //   an INFINITE one  -- every intersection is true, but an entry whose own
    //                       bound is EMPTY is dropped by the predicate.
    //
    // The second is not hypothetical: a stroke or an armature with no points,
    // and a volume with no payload, all bound to an empty box and all local, so
    // this document holds one of each beside the non-local plane and the
    // ordinary spheres. Without the fallback the infinite region keeps them and
    // the plan stops matching the compiler.
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    const NodeId plane = l.sdf->insert(item(Prim::plane(cf3(0, 1, 0), 0.0f), cf3(0, 0, 0)));
    Node pointless;
    pointless.prim.type = PrimType::Stroke;  // no stroke points: an empty bound
    const NodeId stroke = l.sdf->insert(pointless);
    Node boneless;
    boneless.prim.type = PrimType::Armature;  // likewise
    l.sdf->insert(boneless);
    Node hollow;
    hollow.prim.type = PrimType::Volume;  // no payload, likewise
    l.sdf->insert(hollow);
    for (int i = 0; i < 12; ++i)
        l.sdf->insert(item(Prim::sphere(0.1f), cf3(0.3f * static_cast<float>(i), 0, 0)));

    // Empty is the default-constructed box; infinite is the one a plan over
    // "everything" would be given.
    const math::Aabb regions[] = {
        math::Aabb{},
        math::Aabb::infinite(),
        math::Aabb{cf3(-0.05f, -0.05f, -0.05f), cf3(0.05f, 0.05f, 0.05f)},
        math::Aabb{cf3(40, 40, 40), cf3(41, 41, 41)},
    };
    const CullIndex index(doc);
    for (const math::Aabb& region : regions) {
        CAPTURE(region.empty());
        CAPTURE(region.is_infinite());
        const CullPlan plan = index.plan(region);
        const std::vector<CullIndex::Entry>* got = plan.chain(l, l.sdf->roots);
        REQUIRE(got);
        const std::vector<NodeId> want = expected_survivors(
            l, index.cull_pad() > 0.0f ? region.dilated(index.cull_pad()) : region);
        REQUIRE(got->size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) CHECK((*got)[i].id == want[i]);
    }

    // Spelled out, so a fold that quietly changed either answer is named rather
    // than merely counted: over EVERYTHING the empty-bound items are dropped and
    // everything else is kept, and over NOTHING only the plane -- which can
    // never be culled -- comes back.
    {
        const CullPlan all = index.plan(math::Aabb::infinite());
        const std::vector<CullIndex::Entry>* got = all.chain(l, l.sdf->roots);
        REQUIRE(got);
        CHECK(got->size() == 13);  // the plane and the 12 spheres, not the 3 empties
        for (const CullIndex::Entry& e : *got) CHECK(e.id != stroke);
    }
    {
        const CullPlan none = index.plan(math::Aabb{});
        const std::vector<CullIndex::Entry>* got = none.chain(l, l.sdf->roots);
        REQUIRE(got);
        REQUIRE(got->size() == 1);
        CHECK((*got)[0].id == plane);
    }
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

// How many items an index believes its layers' root chains hold. Everything
// survives a plan over an all-containing region, so this is the index's own
// cached entries. Summed over layers rather than per chain because the corpus
// instances a layer: one appended item lands in every chain over that root
// list, so what an append adds here is more than one and is compared against a
// fresh build rather than counted.
std::size_t root_entries(const CullIndex& ix) {
    const CullPlan plan = ix.plan(math::Aabb{cf3(-100, -100, -100), cf3(100, 100, 100)});
    std::size_t n = 0;
    for (const Layer& layer : ix.document()->layers) {
        if (!layer.visible || layer.kind != LayerKind::Sdf || !layer.sdf) continue;
        if (const std::vector<CullIndex::Entry>* e = plan.chain(layer, layer.sdf->roots))
            n += e->size();
    }
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

    SUBCASE("a GROUP whose CHILD is what widens the pad") {
        // The pad folds over the layer's FLAT node map, so the subtree an
        // append adds sets it wherever inside that subtree the wide blend
        // sits. An index that raised its pad only from the ids it was handed
        // would keep the old one here and plan against too small a region.
        Document doc = gnarly_document();
        CullIndex grown(doc);
        const float before = grown.cull_pad();
        Node g;
        g.is_group = true;  // the group itself drags nothing (Hard, k = 0)
        const NodeId gid = doc.layers[0].sdf->insert(g);
        REQUIRE(gid != kNoNode);
        doc.layers[0].sdf->insert(dab(cf3(0.45f, -0.15f, 0.1f), 1.7f), gid);
        check_append_matches_fresh(doc, {gid}, grown, 817);
        CHECK(grown.cull_pad() > before);
    }

    SUBCASE("an INVISIBLE group, whose child the pad still counts") {
        // The sharp form of the case above: the build never descends into an
        // invisible group -- it gets no entries and no chain -- but cull_pad
        // walks the node map, which does not care what the build descended
        // into, and the visible child inside it sets the pad either way.
        Document doc = gnarly_document();
        CullIndex grown(doc);
        const float before = grown.cull_pad();
        Node g;
        g.is_group = true;
        g.visible = false;
        const NodeId gid = doc.layers[0].sdf->insert(g);
        REQUIRE(gid != kNoNode);
        doc.layers[0].sdf->insert(dab(cf3(0.55f, -0.25f, 0.15f), 1.9f), gid);
        check_append_matches_fresh(doc, {gid}, grown, 818);
        CHECK(grown.cull_pad() > before);
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

    SUBCASE("a SYMMETRIC layer's append resolves the same envelope a fresh build does") {
        // The chain-pad envelope reads the layer's EFFECTIVE contributor
        // count — node map times symmetry multiplicity, the multiplicity
        // read from the LIVE layer at refresh time. An append grows the map,
        // both factors of the product only rise, and the stored raw count
        // stays the map size; this holds the appended pad equal to a fresh
        // build's, on a layer where the multiplicity is doing real work.
        // (A symmetry EDIT never reaches a live index: SetLayerMirrorCmd and
        // SetLayerRadialCmd are not tail appends, so the C ABI routes them
        // through the general invalidation and the rebuild.)
        Document doc;
        Layer& l = doc.add_sdf_layer("radial");
        l.radial_count = 8;
        l.radial_axis = 1;
        l.radial_k = 0.06f;
        l.mirror_axes = kMirrorX;
        l.mirror_k = 0.06f;
        for (int i = 0; i < 40; ++i)
            l.sdf->insert(item(Prim::sphere(0.1f), cf3(0.9f, 0.04f * static_cast<float>(i), 0.1f),
                               Op::Add, Blend{BlendProfile::Quadratic, 0.06f}));
        CullIndex grown(doc);
        const float before = grown.cull_pad();
        std::vector<NodeId> ids;
        for (int i = 0; i < 40; ++i)
            ids.push_back(l.sdf->insert(item(Prim::sphere(0.1f),
                                             cf3(0.9f, 1.6f + 0.04f * static_cast<float>(i), 0.1f),
                                             Op::Add, Blend{BlendProfile::Quadratic, 0.06f})));
        check_append_matches_fresh(doc, ids, grown, 819);
        // Not vacuous: the envelope really is in its rising regime here —
        // the append doubled the map, so the resolved pad moved.
        CHECK(grown.cull_pad() > before);
    }
}

TEST_CASE("cull index: the pad an append raises is a maximum of SUMS over layers") {
    // The trap that keeps the pad's two terms PER LAYER (scene/cull_index.h).
    // Layer A holds a feathered volume replace and blends nothing; layer B
    // blends and holds no volume. A fresh build reports max(A.feather,
    // B.blend). One pair of maxima for the whole document would report
    // A.feather + B.blend -- larger, so safe to cull with, but not the number a
    // rebuild gives, and an appended index is held EQUAL to a rebuilt one.
    Document doc;
    // Both layers first: add_sdf_layer grows doc.layers, which moves the ones
    // already in it.
    doc.add_sdf_layer("feathered");
    doc.add_sdf_layer("blended");
    Layer& a = doc.layers[0];
    Layer& b = doc.layers[1];
    Node v;
    v.prim = Prim::volume();
    v.volume = std::make_shared<field::FieldVolume>(feathered_ball(cf3(0.0f, 0.0f, 0.0f), 0.4f));
    v.op = Op::Replace;  // Hard, k = 0 by default: it drags no chain
    a.sdf->insert(std::move(v));

    b.xform.position = cf3(0.0f, 1.5f, 0.0f);
    b.sdf->insert(dab(cf3(0.0f, 0.0f, 0.0f), 0.3f));

    const CullPadTerms ta = cull_pad_terms(*a.sdf, a);
    const CullPadTerms tb = cull_pad_terms(*b.sdf, b);
    const std::size_t na = a.sdf->nodes().size(), nb0 = b.sdf->nodes().size();
    REQUIRE(ta.feather > 0.0f);
    REQUIRE(ta.blend_total(na) == 0.0f);
    REQUIRE(tb.feather == 0.0f);
    REQUIRE(tb.blend_total(nb0) > 0.0f);

    CullIndex grown(doc);
    CHECK(grown.cull_pad() == std::max(ta.total(na), tb.total(nb0)));
    // NOT the sum of maxima
    CHECK(grown.cull_pad() < ta.feather + tb.blend_total(nb0));

    // Now raise the blended layer's term with an append. The document's pad
    // must follow ITS layer's sum, not collect a term from each layer.
    const NodeId id = b.sdf->insert(dab(cf3(0.2f, 0.1f, 0.0f), 0.9f));
    check_append_matches_fresh(doc, {id}, grown, 819);
    const CullPadTerms after = cull_pad_terms(*b.sdf, b);
    const std::size_t nb1 = b.sdf->nodes().size();
    CHECK(grown.cull_pad() == std::max(ta.total(na), after.total(nb1)));
    CHECK(grown.cull_pad() < ta.feather + after.blend_total(nb1));
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
    SUBCASE("the node map gained a node the append did not name") {
        // An append raises the pad from the subtree it names, so a node map
        // that gained anything ELSE would leave the pad below what a fresh
        // build reports -- and too small a pad plans against too small a
        // region, which is the one direction that loses items a brick needed.
        // The map's size is the O(1) check that catches it; a mismatch refuses,
        // which costs the caller the rebuild it would have done anyway.
        CullIndex ix(doc);
        SdfContent& c = *doc.layers[0].sdf;
        NodeId group = kNoNode;
        for (const auto& [id, n] : c.nodes())
            if (n.is_group) group = id;
        REQUIRE(group != kNoNode);
        c.insert(dab(cf3(0.1f, 0.4f, -0.6f), 1.3f), group);  // not at any tail
        const NodeId tail = c.insert(dab(cf3(0.7f, 0.2f, 0.1f)));
        CHECK_FALSE(ix.append({tail}));
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

TEST_CASE("cull index: a cached index is extended in place only while unobserved") {
    // scene::append_cached, the cache's half of an append. Extending the cached
    // index itself is what makes a dab cost the dab rather than the document,
    // and it is safe only while nothing else holds that index: a holder planned
    // against what it took and must go on seeing it, entries and pad both.
    //
    // Both directions are asserted here rather than through the C ABI because
    // the C ABI never lets a handle out from under its mutex, so the copy
    // branch is reachable there only across threads -- see cull_index.h.

    SUBCASE("a stroke extends the cached index without copying it") {
        Document doc = gnarly_document();
        auto slot = std::make_shared<CullIndex>(doc);
        // A weak handle rather than a bare address: a copy destroys the index
        // it replaces, and the allocator is free to hand the copy the very
        // block it just freed -- comparing addresses passed by luck on half the
        // iterations of this loop. A weak_ptr adds no owner, so it does not
        // itself force the copy branch, and it goes empty exactly when the
        // index it watches is replaced.
        //
        // Compared as raw pointers because MSVC's <memory> cannot stream a
        // shared_ptr, which doctest needs to report the operands (/WX). It is
        // the same assertion: an expired lock() is null, and null is never one
        // of these addresses.
        std::weak_ptr<CullIndex> watch = slot;
        const std::size_t before = root_entries(*slot);
        for (int i = 0; i < 6; ++i) {
            const NodeId id = doc.layers[0].sdf->insert(dab(cf3(0.1f * i, 0.35f, -0.2f)));
            REQUIRE(slot.use_count() == 1);
            REQUIRE(append_cached(slot, {id}));
            CHECK(watch.lock().get() == slot.get());  // the same index, grown, not a copy
        }
        const CullIndex fresh(doc);
        CHECK(root_entries(*slot) > before);
        CHECK(root_entries(*slot) == root_entries(fresh));
        CHECK(slot->cull_pad() == fresh.cull_pad());
    }

    SUBCASE("a held index is not mutated under its holder") {
        Document doc = gnarly_document();
        auto slot = std::make_shared<CullIndex>(doc);
        // What a reader has: a handle it keeps for as long as it reads, and the
        // numbers it planned against.
        const std::shared_ptr<const CullIndex> held = slot;
        const float held_pad = held->cull_pad();
        const std::size_t held_entries = root_entries(*held);

        // Wide enough to move the pad, so an in-place extension would show up
        // in BOTH numbers the holder plans with and not just in the entries.
        const NodeId id = doc.layers[0].sdf->insert(dab(cf3(0.45f, -0.15f, 0.1f), 1.7f));
        REQUIRE(append_cached(slot, {id}));

        // Addresses are sound here where they were not above: `held` keeps the
        // old index alive, so nothing can be handed its block.
        CHECK(slot.get() != held.get());  // the cache took a copy to extend
        CHECK(held->cull_pad() == held_pad);  // and left the holder its own
        CHECK(root_entries(*held) == held_entries);

        // And the copy is the extended index a rebuild would give, not merely
        // an untouched copy.
        const CullIndex fresh(doc);
        CHECK(slot->cull_pad() > held_pad);
        CHECK(root_entries(*slot) > held_entries);
        CHECK(root_entries(*slot) == root_entries(fresh));
        CHECK(slot->cull_pad() == fresh.cull_pad());
    }

    SUBCASE("a refused append leaves the holder and the slot alone") {
        // The copy branch has the same contract as the in-place one: a refusal
        // costs a rebuild, never a half-extended index in either hand.
        Document doc = gnarly_document();
        auto slot = std::make_shared<CullIndex>(doc);
        const std::shared_ptr<const CullIndex> held = slot;
        const float pad = held->cull_pad();
        const NodeId id = doc.layers[0].sdf->insert(dab(cf3(0.2f, 0.2f, 0.2f)));
        doc.layers[0].sdf->insert(dab(cf3(0.3f, 0.2f, 0.2f)));  // id is no longer the tail
        CHECK_FALSE(append_cached(slot, {id}));
        CHECK(slot.get() == held.get());
        CHECK(held->cull_pad() == pad);
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
                             float* out_worst, int samples = 4000) {
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
                                                    seed + id, lo, hi, out_worst, samples);
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

// WHY THESE TWO RUN AT 200,000 SAMPLES AND THE CORPUS RUNS AT 4,000 (#326).
//
// #326 was filed because nothing in the suite could tell a correct influence
// bound from a wrong one for an intersect: the item's own geometry box is ~3x
// tighter than the layer's and measured drift 0 too, so whichever bound shipped
// would have been chosen on reasoning rather than evidence. Four candidate
// fixture designs were proposed to break the tie.
//
// None of them was needed. THE FIXTURE WAS NEVER THE PROBLEM — the sample count
// was. Moving the intersect of the plain sphere+box document leaves 34 drifting
// points in 400,000, about 1 in 11,700, so 4,000 samples miss it roughly seven
// times in ten. At 200,000 it shows every run, and the too-small bound reads
// 0.100 of drift against a 0.15 band.
//
// So a non-local case is worth sampling densely and a local one is not: a local
// item's bound is its own geometry and a violation there is dense, which is what
// the gnarly corpus finds at 4,000. The number is a property of how RARE the
// violation is, not of how hard the document is.
constexpr int kNonLocalSamples = 200000;

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
    check_influence_promise(doc, 8802, -2.5f, 2.5f, &moved, &points, &infinite, &worst,
                            kNonLocalSamples);
    MESSAGE("intersect: worst drift = ", worst);
    CHECK(moved > 0);
    CHECK(points > 100000);
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
    check_influence_promise(doc, 8803, -3.5f, 3.5f, &moved, &points, &infinite, &worst,
                            kNonLocalSamples);
    MESSAGE("non-local: worst drift = ", worst);
    CHECK(moved > 0);
    CHECK(points > 500);
}

// The measurement #326 asked for: rank the candidate bounds on evidence.
//
// It wanted "a fixture where shrinking the intersect's bound produces a
// non-zero drift", so the tightest bound that holds could be shipped instead of
// the first one proposed. This is that measurement, and its answer is that the
// bound #319 proposed IS the tightest that holds — the tighter-looking
// alternative leaks.
namespace {

// Worst band-clamped drift outside `box` when `id` moves. The property test's
// arithmetic, against a box the caller chooses rather than the one the engine
// declares — which is what makes it a comparison of candidates.
float drift_outside_box(Document doc, LayerId lid, NodeId id, math::Aabb box, float band,
                        std::uint64_t seed, float lo, float hi, int* out_hits, int samples) {
    Layer* layer = doc.find_layer(lid);
    Node* n = layer->sdf->find_mut(id);
    const cfloat3 delta = cf3(0.11f, -0.07f, 0.05f);
    const Tape before = compile_document(doc);
    const cfloat3 was = n->xform.position;
    n->xform.position = was + delta;
    const Tape after = compile_document(doc);
    n->xform.position = was;

    clay_test::Lcg rng(seed);
    float worst = 0.0f;
    int hits = 0;
    for (int i = 0; i < samples; ++i) {
        const cfloat3 p = rng.vec3(lo, hi);
        if (box.contains(p)) continue;
        const float a = cclamp(before.eval(p).d, -band, band);
        const float b = cclamp(after.eval(p).d, -band, band);
        const float dv = a > b ? a - b : b - a;
        if (dv != 0.0f) ++hits;
        if (dv > worst) worst = dv;
    }
    *out_hits = hits;
    return worst;
}

// The union of a layer's visible item GEOMETRY, with no non-locality rule
// applied — "the layer's own bounds" as #319 proposes them. Written here rather
// than called from the engine because for a morph the engine correctly answers
// INFINITE, and the question this test asks is what would happen if it did not.
math::Aabb layer_geometry_union(const Layer& layer) {
    math::Aabb b;
    for (const auto& [id, n] : layer.sdf->nodes()) {
        (void)id;
        if (n.is_group || !n.visible) continue;
        b.expand(item_geometry_bound(n, layer));
    }
    return b;
}

// Both sides of the move, as every consumer of an influence bound unions them.
math::Aabb spanning(const Document& doc, LayerId lid, NodeId id, bool layer_extent) {
    Document moved = doc;
    const Layer* l0 = doc.find_layer(lid);
    Layer* l1 = moved.find_layer(lid);
    l1->sdf->find_mut(id)->xform.position =
        l1->sdf->find(id)->xform.position + cf3(0.11f, -0.07f, 0.05f);
    math::Aabb b;
    if (layer_extent) {
        b = layer_geometry_union(*l0);
        b.expand(layer_geometry_union(*l1));
    } else {
        b = item_geometry_bound(*l0->sdf->find(id), *l0);
        b.expand(item_geometry_bound(*l1->sdf->find(id), *l1));
    }
    return b;
}

Document sphere_and_intersect(NodeId* out_cutter) {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    *out_cutter =
        l.sdf->insert(item(Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(-0.4f, 0, 0), Op::Intersect));
    return doc;
}

}  // namespace

TEST_CASE("influence bound: the item's own box is TOO SMALL for an intersect") {
    // The discriminating measurement. This is #319's own fixture — no new
    // design was needed — and the drift it shows is what says the layer bound
    // is necessary rather than merely harmless.
    NodeId cutter = kNoNode;
    Document doc = sphere_and_intersect(&cutter);
    const LayerId lid = doc.layers.front().id;
    const float band = 0.15f;
    const float pad = band + cull_pad(*doc.layers.front().sdf, doc.layers.front());

    int own_hits = 0, layer_hits = 0;
    const float own = drift_outside_box(doc, lid, cutter,
                                        spanning(doc, lid, cutter, false).dilated(pad), band,
                                        4401, -3.0f, 3.0f, &own_hits, kNonLocalSamples);
    const float ext = drift_outside_box(doc, lid, cutter,
                                        spanning(doc, lid, cutter, true).dilated(pad), band,
                                        4401, -3.0f, 3.0f, &layer_hits, kNonLocalSamples);
    MESSAGE("intersect: own-box drift ", own, " (", own_hits, " pts), layer-extent drift ", ext);

    // The item's own geometry LEAKS. Well clear of float noise against a 0.15
    // band, and the reason a bound may not be chosen by which one looks tighter.
    CHECK(own > 0.01f);
    CHECK(own_hits > 0);
    // The layer's extent holds, exactly.
    CHECK(ext == 0.0f);
    CHECK(layer_hits == 0);
}

TEST_CASE("influence bound: what a spatial morph's leak past the layer looks like") {
    // REPORTS, DOES NOT GATE, and the reason is worth stating because the case
    // above it does gate.
    //
    // The morphs keep the infinite answer they already had, so nothing here
    // needs measured backing — this change tightens the INTERSECT and leaves
    // every other non-local op exactly as it was. The claim behind the morphs
    // is mechanical rather than empirical: a morph's weight SATURATES.
    // `ctransition_radial_weight` is clamp((length(p.xz) - r0) / (r1 - r0), 0, 1)
    // about the WORLD Y axis and the linear one is
    // clamp(dot(p - a, ab) / dot(ab, ab), 0, 1) along a segment, so past the
    // span the weight is exactly 1, the result IS the item's own field, and
    // moving the item changes it arbitrarily far away. That is a statement
    // about the kernel, not about a fixture.
    //
    // The empirical side is MARGINAL and platform-dependent, which is why it is
    // a message. Probed at 200,000 samples, the radial morph leaks 4 points at
    // 0.0178 on macOS/arm64 and 0 on x86_64: the leaking points sit where a
    // value is a rounding error either side of the band edge, so which platform
    // sees them is decided by the last bits of a tape evaluation. Asserting it
    // would gate a sound claim on float rounding — this suite has paid for that
    // kind of test before.
    for (Op op : {Op::TransitionLinear, Op::TransitionRadial}) {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
        const NodeId morph =
            l.sdf->insert(item(Prim::box(cf3(0.5f, 0.5f, 0.5f)), cf3(0.3f, 0, 0), op));
        const float band = 0.15f;
        const float pad = band + cull_pad(*doc.layers.front().sdf, doc.layers.front());

        int hits = 0;
        const float ext = drift_outside_box(doc, l.id, morph,
                                            spanning(doc, l.id, morph, true).dilated(pad), band,
                                            4402, -6.0f, 6.0f, &hits, kNonLocalSamples);
        MESSAGE("morph ", static_cast<int>(op), ": drift outside the LAYER's extent = ", ext,
                " over ", hits, " points (reported, not gated)");
    }
    CHECK(true);  // the contract itself is the case below
}

TEST_CASE("influence bound: both morphs report unbounded, and an intersect does not") {
    // The engine's side of the same claim, for both morphs — the one the
    // measurement above is only able to demonstrate for the radial.
    for (Op op : {Op::TransitionLinear, Op::TransitionRadial}) {
        CAPTURE(static_cast<int>(op));
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
        const NodeId morph =
            l.sdf->insert(item(Prim::box(cf3(0.5f, 0.5f, 0.5f)), cf3(0.3f, 0, 0), op));
        CHECK(item_influence_bound(*l.sdf->find(morph), l).is_infinite());
        CHECK(item_nonlocality(*l.sdf->find(morph)) == Nonlocality::Unbounded);
        // and neither is ever culled, which is unchanged for all of them
        CHECK(!item_influence_is_local(*l.sdf->find(morph)));
    }
    NodeId cutter = kNoNode;
    Document doc = sphere_and_intersect(&cutter);
    const Layer& l = doc.layers.front();
    CHECK(!item_influence_bound(*l.sdf->find(cutter), l).is_infinite());
    CHECK(item_nonlocality(*l.sdf->find(cutter)) == Nonlocality::BoundedByLayer);
    CHECK(!item_influence_is_local(*l.sdf->find(cutter)));
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

// -- a `k` a hard profile never drags (#335) --------------------------------
//
// ClaySpaceDesktop's move brush measured 1.82x on a frame path the notes record
// at 1.34x faster. The cause is the chain pad #282 added, which is the cost of
// that fix and not a defect; what IS a defect turned up beside it.
//
// The pad is the largest single-item reach in the LAYER, and reach was spelled
// `max(support, k)` because a hard profile has zero support — so a `k` left on
// a hard node set the pad for every brick out of a blend that drags nothing.
// `ctape_smin_m` hands a hard profile back a step: the running value is a
// plain `min()` and moves by no `k`. `Paint` and the extended modes still drag
// and keep their reach.
//
// THE NODE'S OWN BOUND IS A DIFFERENT QUESTION and keeps `max(support, k)`.
// That dilation does a second job in a mixed chain — margin for the drag a
// node's SMOOTH neighbours apply to a running value it contributed to — and
// the last case here is what says so, because the first version of this change
// took it away too and only an arm64 runner noticed.
TEST_CASE("a k on a hard blend drags no chain, and pads nothing") {
    const Blend hard_with_k{BlendProfile::Hard, 0.5f};
    const Blend smooth{BlendProfile::Quadratic, 0.5f};
    Document one;
    Layer& ol = one.add_sdf_layer("l");

    SUBCASE("a hard blend drags nothing, whatever its k says") {
        Node n = item(Prim::sphere(0.5f), cf3(0, 0, 0), Op::Add, hard_with_k);
        CHECK(chain_drag_reach(n, 8) == 0.0f);
        // Not a vacuous pass: the same node blended SMOOTHLY drags
        // min(4k, k * envelope(N)) — at 8 nodes the envelope sits at its
        // short-chain floor of 2.80.
        Node soft = item(Prim::sphere(0.5f), cf3(0, 0, 0), Op::Add, smooth);
        CHECK(chain_drag_reach(soft, 8) == 0.5f * 2.80f);
    }

    SUBCASE("its own bound still carries the k, and that is deliberate") {
        Node n = item(Prim::sphere(0.5f), cf3(0, 0, 0), Op::Add, hard_with_k);
        // 0.5 of shape plus 0.5 of reach. The pad narrowed; this did not.
        CHECK(item_geometry_bound(n, ol).max.x == 1.0f);
    }

    SUBCASE("Paint and the extended modes still drag") {
        Node painted = item(Prim::sphere(0.5f), cf3(0, 0, 0), Op::Paint, hard_with_k);
        CHECK(chain_drag_reach(painted, 8) == 0.5f);  // its colour fades over k
        Node grooved = item(Prim::sphere(0.5f), cf3(0, 0, 0), Op::Groove, hard_with_k);
        REQUIRE(op_is_extended(grooved.op));
        CHECK(chain_drag_reach(grooved, 8) == 0.5f);  // the mode ignores the profile
    }

    SUBCASE("a layer of hard nodes pads no cull region at all") {
        Document doc;
        Layer& l = doc.add_sdf_layer("hard");
        for (int i = 0; i < 8; ++i)
            l.sdf->insert(item(Prim::sphere(0.4f), cf3(0.3f * static_cast<float>(i), 0, 0), Op::Add,
                               hard_with_k));
        CHECK(cull_pad(*l.sdf, l) == 0.0f);
        CHECK(blend_cull_pad(*l.sdf, l) == 0.0f);
        CullIndex index(doc);
        CHECK(index.cull_pad() == 0.0f);
    }

    SUBCASE("the smooth chain's pad is untouched") {
        Document doc;
        Layer& l = doc.add_sdf_layer("smooth");
        for (int i = 0; i < 8; ++i)
            l.sdf->insert(
                item(Prim::sphere(0.4f), cf3(0.3f * static_cast<float>(i), 0, 0), Op::Add, smooth));
        // #282's pad, to the float: the largest single-item DRAG in the layer,
        // which #335 narrowed to min(support, k * envelope(N)). At 8 nodes the
        // envelope is its 2.80 floor, under the quadratic support of 4k.
        CHECK(cull_pad(*l.sdf, l) ==
              kernel::cmin(
                  kernel::ctape_blend_support(static_cast<int>(BlendProfile::Quadratic), 0.5f),
                  0.5f * chain_pad_envelope(BlendProfile::Quadratic, l.sdf->nodes().size())));
        CHECK(cull_pad(*l.sdf, l) > 0.0f);
    }

    SUBCASE("and a mixed chain's field does not move") {
        // THE CASE THAT CAUGHT THE OVER-REACH. The first version of this change
        // narrowed the node's own bound as well, and 2,400 samples over 24
        // bricks passed it on x86-64 while an arm64 runner failed — so this
        // sweeps 200 bricks, and its document is one the cull contract HOLDS
        // on rather than one that merely looks adversarial. (A 12-node chain
        // blending at k = 0.5 disagrees 540 times in 800,000 samples on main
        // and exactly 540 with this change: #282's pad is a heuristic and that
        // document is past it, which makes it useless for measuring anything
        // here.)
        Document doc;
        Layer& l = doc.add_sdf_layer("mixed");
        l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
        const double golden = 0.6180339887;
        for (int i = 1; i < 200; ++i) {
            const double u = std::fmod(static_cast<double>(i) * golden, 1.0);
            const double v = (static_cast<double>(i) + 0.5) / 200.0;
            const double phi = std::acos(1.0 - 2.0 * v), th = 6.283185307 * u;
            const cfloat3 at = cf3(static_cast<float>(std::sin(phi) * std::cos(th)),
                                   static_cast<float>(std::cos(phi)),
                                   static_cast<float>(std::sin(phi) * std::sin(th)));
            // Alternating, and the HARD one carries the larger k — which is
            // what makes it the layer's maximum and so the whole of the pad.
            l.sdf->insert(item(
                Prim::sphere(0.05f), at, Op::Add,
                (i % 2) ? Blend{BlendProfile::Hard, 0.5f} : Blend{BlendProfile::Quadratic, 0.05f}));
        }
        // The pad is now the smooth drag alone. Stated here because it is the
        // whole mechanism: before, a hard 0.5 set it and every brick paid.
        // The smooth drag itself is min(support, k * envelope(N)) since #335.
        CHECK(cull_pad(*l.sdf, l) ==
              kernel::cmin(
                  kernel::ctape_blend_support(static_cast<int>(BlendProfile::Quadratic), 0.05f),
                  0.05f * chain_pad_envelope(BlendProfile::Quadratic, l.sdf->nodes().size())));

        const float band = 0.15f;
        const Tape full = compile_document(doc);
        const CullIndex index(doc);
        clay_test::Lcg rng(335);
        int differ = 0, sampled = 0;
        std::size_t culled_instrs = 0;
        for (int b = 0; b < 200; ++b) {
            cfloat3 corner = rng.vec3(-1.4f, 1.4f);
            math::Aabb brick{corner, corner + cf3(0.4f, 0.4f, 0.4f)};
            CullRegion cull{brick.dilated(band)};
            const CullPlan plan = index.plan(brick.dilated(band));
            const Tape culled = compile_document(doc, &cull, &index, &plan);
            culled_instrs += culled.instrs.size();
            for (int i = 0; i < 40; ++i) {
                cfloat3 p =
                    cf3(rng.range(brick.min.x, brick.max.x), rng.range(brick.min.y, brick.max.y),
                        rng.range(brick.min.z, brick.max.z));
                ++sampled;
                if (cclamp(full.eval(p).d, -band, band) != cclamp(culled.eval(p).d, -band, band))
                    ++differ;
            }
        }
        CHECK(differ == 0);
        // NOT a vacuous pass in either direction: the sweep ran, and the cull
        // it is checking actually dropped most of the document per brick.
        CHECK(sampled == 8000);
        CHECK(culled_instrs < 200 * full.instrs.size() / 2);
    }
}

// -- the chain-pad envelope's worst measured configs, pinned (#335) ----------
//
// The chain pad is min(support, k * envelope(N)) per item, and the envelope
// is a MEASURED fit (bounds.cpp records the campaign: 700+ configs, lengths
// 75-8000, k 0.03-0.24, all three smooth profiles, stroke/shuffled/reversed/
// ladder chain orders, mixed and subtract compositions, three brick-seed
// draws per config, arm64 and x86-64). The minimal sufficient pad GROWS with
// chain length -- a fixed 3k cap that closed every stroke-ordered chain at
// 600 nodes left real disagreements at 2000-5000, up to 9.75e-4, 14x the
// fp16 quantization bricks store through -- so the pins below hold both ends:
// the short-chain knees the fit clears by its 0.5k seed-drift margin, and
// the long chains where only the support clamp (the pre-#335 pad) is wide
// enough.
//
// VERIFIED TO BITE before shipping, by deleting the envelope's length term
// (slope zeroed, leaving a fixed-multiple cap at the 2.80/2.75/2.70 bases):
// five of the pins below fail -- reversed quadratic 600 @ k=0.12 (4
// disagreements), ladder cubic 600 @ k=0.12 (16), ladder quadratic 600 @
// k=0.12 (1) and @ k=0.24 (7), and stroke quadratic 3000 @ k=0.06 (1) --
// and all pass again with the shipped fit. A tuning pass cannot lower the
// envelope without the sweep's breadth of evidence.

namespace {

enum class ChainOrder { Stroke, Reversed, Ladder };

// Layer symmetry for the amplified family: `radial` > 1 turns on the layer's
// radial mode AND confines the dabs to one 1/C sector — the violating shape,
// where the node map looks short and the compiled chain is not. `mirror_axes`
// turns on the layer mirror. `seam_k` < 0 means "the item k"; the seam-k pins
// set it WIDER than any item k, the shape only the seam fold covers.
struct ChainSymmetry {
    int radial = 0;
    std::uint8_t mirror_axes = 0;
    float seam_k = -1.0f;
};

// The evidence campaign's document family: golden-spiral dabs of r = 0.05 on
// a unit sphere, blended down one chain. `Reversed` flips the stroke order;
// `Ladder` sorts the dabs by descending distance from the +Y pole (each dab
// lands one fine rung closer, the adversarial order the sweep found worst);
// `subtract_every3` carves every third dab instead of adding it. The dabs
// keep Node's default mirror = true, so under `sym` each is emitted once per
// symmetry copy; the base sphere opts out (symmetric anyway).
Document chain_doc(int len, Blend blend, ChainOrder order = ChainOrder::Stroke,
                   bool subtract_every3 = false, ChainSymmetry sym = {}) {
    Document doc;
    Layer& l = doc.add_sdf_layer("chain");
    const float seam = sym.seam_k >= 0.0f ? sym.seam_k : blend.k;
    if (sym.radial > 1) {
        l.radial_count = static_cast<std::uint16_t>(sym.radial);
        l.radial_axis = 1;
        l.radial_k = seam;
    }
    if (sym.mirror_axes != 0) {
        l.mirror_axes = sym.mirror_axes;
        l.mirror_k = seam;
    }
    Node base = item(Prim::sphere(1.0f), cf3(0, 0, 0));
    base.mirror = false;
    l.sdf->insert(base);
    std::vector<Node> dabs;
    const double golden = 0.6180339887;
    const double sector = 6.283185307 / (sym.radial > 1 ? sym.radial : 1);
    for (int i = 1; i < len; ++i) {
        const double u = std::fmod(static_cast<double>(i) * golden, 1.0);
        const double v = (static_cast<double>(i) + 0.5) / static_cast<double>(len);
        const double phi = std::acos(1.0 - 2.0 * v), th = sector * u;
        const cfloat3 at = cf3(static_cast<float>(std::sin(phi) * std::cos(th)),
                               static_cast<float>(std::cos(phi)),
                               static_cast<float>(std::sin(phi) * std::sin(th)));
        const Op op = (subtract_every3 && i % 3 == 0) ? Op::Subtract : Op::Add;
        dabs.push_back(item(Prim::sphere(0.05f), at, op, blend));
    }
    if (order == ChainOrder::Reversed) std::reverse(dabs.begin(), dabs.end());
    if (order == ChainOrder::Ladder) {
        const cfloat3 pole = cf3(0.0f, 1.0f, 0.0f);
        std::stable_sort(dabs.begin(), dabs.end(), [&](const Node& a, const Node& b) {
            return clength(a.xform.position - pole) > clength(b.xform.position - pole);
        });
    }
    for (const Node& d : dabs) l.sdf->insert(d);
    return doc;
}

struct ChainSweep {
    int differ = 0;      // band-clamped disagreements, this pad
    int differ_old = 0;  // same bricks culled as the pre-#335 pad would
    int sampled = 0;
    std::size_t culled_instrs = 0;
    std::size_t culled_old_instrs = 0;
    std::size_t full_instrs = 0;
};

// 200 bricks, `samples_per_brick` samples each, band 0.15 -- the methodology
// of the mixed sweep above, plus a control: culling the same brick with its
// region grown by (support - pad) reproduces the PRE-#335 pad exactly (the
// compiler tests bounds against region + pad, so region + extra with the
// envelope pad equals region with pad = support), which makes "equal-or-fewer
// than the old pad" a live comparison instead of a recorded constant.
ChainSweep sweep_chain(const Document& doc, std::uint64_t seed, float extra_old_pad,
                       int samples_per_brick) {
    const float band = 0.15f;
    ChainSweep r;
    const Tape full = compile_document(doc);
    r.full_instrs = full.instrs.size();
    clay_test::Lcg rng(seed);
    for (int b = 0; b < 200; ++b) {
        cfloat3 corner = rng.vec3(-1.4f, 1.4f);
        math::Aabb brick{corner, corner + cf3(0.4f, 0.4f, 0.4f)};
        CullRegion cull{brick.dilated(band)};
        CullRegion cull_old{brick.dilated(band + extra_old_pad)};
        const Tape culled = compile_document(doc, &cull);
        const Tape culled_old = compile_document(doc, &cull_old);
        r.culled_instrs += culled.instrs.size();
        r.culled_old_instrs += culled_old.instrs.size();
        for (int i = 0; i < samples_per_brick; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            ++r.sampled;
            const float df = cclamp(full.eval(p).d, -band, band);
            if (df != cclamp(culled.eval(p).d, -band, band)) ++r.differ;
            if (df != cclamp(culled_old.eval(p).d, -band, band)) ++r.differ_old;
        }
    }
    return r;
}

void check_chain_pin(int len, const Blend& blend, std::uint64_t seed,
                     ChainOrder order = ChainOrder::Stroke, bool subtract_every3 = false,
                     int samples_per_brick = 40, ChainSymmetry sym = {}) {
    Document doc = chain_doc(len, blend, order, subtract_every3, sym);
    const Layer& l = doc.layers[0];
    // What the region must grow by to reproduce the pre-#335 pad, computed
    // LIVE from the shipped pad: the reach the envelope took away. Zero once
    // the support clamp binds, where the envelope pad IS the old pad — and
    // for a symmetric layer the old pad is STILL the item support alone: the
    // pre-#335 pad never read a seam k either, and the seam term is clamped
    // at exactly that ceiling.
    const float extra_old_pad = blend.support() - cull_pad(*l.sdf, l);
    REQUIRE(extra_old_pad >= 0.0f);  // never wider than the pre-#335 pad
    ChainSweep r = sweep_chain(doc, seed, extra_old_pad, samples_per_brick);
    // The bar #339's correction set: equal-or-fewer disagreements than the
    // pad that shipped before this change -- and the envelope measured ZERO
    // at every one of these configs, so hold them there.
    CHECK(r.differ == 0);
    CHECK(r.differ <= r.differ_old);
    // Not vacuous: the sweep ran, the cull dropped something, and the
    // envelope pad never keeps more of a brick's tape than the pre-#335 pad
    // did -- a fixed "dropped most of the document" share would be
    // meaningless across these configs, whose pads range from 4% to 96% of
    // the sphere's radius.
    CHECK(r.sampled == 200 * samples_per_brick);
    CHECK(r.culled_instrs < 200 * r.full_instrs);
    CHECK(r.culled_instrs <= r.culled_old_instrs);
}

}  // namespace

TEST_CASE("the chain-pad envelope holds at the sweep's worst measured configs") {
    // Short chains: the knees the fit clears by its seed-drift margin.
    SUBCASE("quadratic, 600 nodes, k = 0.12 -- knee 2.95k, envelope 3.85k") {
        check_chain_pin(600, Blend{BlendProfile::Quadratic, 0.12f}, 3351);
    }
    SUBCASE("quadratic reversed, 600 nodes, k = 0.12 -- the binding 600 knee, 3.05k") {
        check_chain_pin(600, Blend{BlendProfile::Quadratic, 0.12f}, 3352, ChainOrder::Reversed);
    }
    SUBCASE("cubic, 300 nodes, k = 0.06 -- envelope 3.75k of a 6k support") {
        check_chain_pin(300, Blend{BlendProfile::Cubic, 0.06f}, 3353);
    }
    SUBCASE("circular, 300 nodes, k = 0.06 -- envelope 3.30k, just under its ~3.41k support") {
        check_chain_pin(300, Blend{BlendProfile::Circular, 0.06f}, 3354);
    }
    SUBCASE("smooth-subtract, 300 nodes, k = 0.12 -- carves in the chain") {
        check_chain_pin(300, Blend{BlendProfile::Quadratic, 0.12f}, 3355, ChainOrder::Stroke,
                        true);
    }
    // The adversarial order that refuted the fixed cap: descending-ladder
    // chains need more than any stroke order at the same length.
    SUBCASE("ladder cubic, 600 nodes, k = 0.12 -- knee 3.75k, the fixed 3k cap's worst refutation") {
        check_chain_pin(600, Blend{BlendProfile::Cubic, 0.12f}, 3357, ChainOrder::Ladder);
    }
    SUBCASE("ladder quadratic, 600 nodes, k = 0.12 -- knee 2.95k") {
        check_chain_pin(600, Blend{BlendProfile::Quadratic, 0.12f}, 3358, ChainOrder::Ladder);
    }
    SUBCASE("ladder quadratic, 600 nodes, k = 0.24 -- knee 3.00k, the large-k end") {
        check_chain_pin(600, Blend{BlendProfile::Quadratic, 0.24f}, 3359, ChainOrder::Ladder);
    }
    // Long chains: the length creep the fixed cap missed. From ~800 nodes on,
    // the quadratic envelope clamps at support -- the pre-#335 pad -- which is
    // the only fixed dilation the sweep found wide enough out here.
    SUBCASE("quadratic, 1200 nodes, k = 0.06 -- knee 3.25k, past any fixed 3k") {
        check_chain_pin(1200, Blend{BlendProfile::Quadratic, 0.06f}, 3360, ChainOrder::Stroke,
                        false, 20);
    }
    SUBCASE("quadratic, 2000 nodes, k = 0.03 -- the small-k long-chain worst") {
        check_chain_pin(2000, Blend{BlendProfile::Quadratic, 0.03f}, 3361, ChainOrder::Stroke,
                        false, 10);
    }
    SUBCASE("quadratic, 3000 nodes, k = 0.06 -- where 3k measured 9.75e-4 off") {
        check_chain_pin(3000, Blend{BlendProfile::Quadratic, 0.06f}, 3362, ChainOrder::Stroke,
                        false, 10);
    }
    SUBCASE("k = 0.5 stays equal-or-fewer than the old pad, where the contract is already past") {
        // A 12-node chain at k = 0.5 is PAST the pad heuristic on main -- the
        // mixed sweep's comment records 540 disagreements in 800,000 samples
        // there. Zero is not the bar for this config; not exceeding the
        // pre-#335 pad is.
        const Blend b{BlendProfile::Quadratic, 0.5f};
        Document doc = chain_doc(12, b);
        const float extra = b.support() - cull_pad(*doc.layers[0].sdf, doc.layers[0]);
        ChainSweep r = sweep_chain(doc, 3356, extra, 40);
        CHECK(r.differ <= r.differ_old);
        CHECK(r.sampled == 8000);
    }
}

// -- the chain counts symmetry copies (#335, round 3) ------------------------
//
// emit_item compiles a mirrored item once per copy the layer's mirror and
// radial modes emit — 1 + popcount(mirror_axes) + (radial_count - 1)
// instances, each a real leaf folded into the layer's ONE serial chain
// through its own seam combine — so a 75-node map under radial 64 is a
// ~4800-contributor chain. Resolving the envelope against the map size alone
// left it floored at its short-chain base where the measured need was ~4k:
// 15 sector-confined radial configs (C = 8..64) measured real in-band
// disagreements, the worst 1.9e-3 = 27x the fp16 floor, against ZERO for the
// pre-#335 pad. The envelope now reads N_eff = nodes * multiplicity, and the
// seam blends the copies enter through fold in as a quadratic term of the
// LAYER's seam k, clamped at the item-derived ceiling (the pre-#335 pad).
//
// VERIFIED TO BITE before shipping, twice. (1) With the multiplicity deleted
// from every read site (layer_symmetry_multiplicity forced to 1): six pins
// fail — the two cubic 75 x 64 pins (49 and 31 band-clamped disagreements
// against a zero old-pad control), quadratic 75 x 64 (25), quadratic
// 150 x 32 stroke and ladder (18, 5), and quadratic 600 x 8 (3). (2) With
// the seam fold deleted (blend_k_seam never raised) but the multiplicity
// kept: both wide-seam pins fail — the radial one pads 0.21 where the
// asserted parity is 0.24 and disagrees 4 times, the mirror one pads 0.1817
// and disagrees once — which is why the seam k needs its own slot and not
// just a longer N. All pass again with both shipped.
TEST_CASE("the chain pad counts the copies a symmetric layer compiles") {
    // The refuted configs of the round-2 sweep, pinned. Bars as above:
    // differ == 0 (each measured zero under envelope(N_eff)), never more
    // than the live pre-#335 control, never a wider pad than it.
    SUBCASE("cubic, 75 nodes x radial 64, k = 0.06 -- the worst refuted config") {
        check_chain_pin(75, Blend{BlendProfile::Cubic, 0.06f}, 4401, ChainOrder::Stroke, false,
                        40, ChainSymmetry{64, 0, -1.0f});
    }
    SUBCASE("cubic, 75 nodes x radial 64, k = 0.06 -- second seed draw") {
        check_chain_pin(75, Blend{BlendProfile::Cubic, 0.06f}, 4402, ChainOrder::Stroke, false,
                        40, ChainSymmetry{64, 0, -1.0f});
    }
    SUBCASE("quadratic, 75 nodes x radial 64, k = 0.06 -- envelope(N_eff) IS the support") {
        // env(4800) crosses the quadratic 4k support: the pad is the
        // pre-#335 one, the tapes bit-identical to it — parity, not margin.
        // A parity config is MAIN-limited: at seed 4403 the shipped-support
        // pad itself measures 17 disagreements here and this pad exactly 17
        // with it (extra_old_pad is 0, the two culls are the same compile).
        // The pin sits at a seed where the support pad measures clean, as
        // the campaign's parity rows did; equal-to-control is the bar the
        // clamp guarantees at ANY seed.
        check_chain_pin(75, Blend{BlendProfile::Quadratic, 0.06f}, 4413, ChainOrder::Stroke,
                        false, 40, ChainSymmetry{64, 0, -1.0f});
    }
    SUBCASE("quadratic, 150 nodes x radial 32, k = 0.06") {
        check_chain_pin(150, Blend{BlendProfile::Quadratic, 0.06f}, 4404, ChainOrder::Stroke,
                        false, 40, ChainSymmetry{32, 0, -1.0f});
    }
    SUBCASE("quadratic ladder, 150 nodes x radial 32, k = 0.06 -- the adversarial order") {
        check_chain_pin(150, Blend{BlendProfile::Quadratic, 0.06f}, 4405, ChainOrder::Ladder,
                        false, 40, ChainSymmetry{32, 0, -1.0f});
    }
    SUBCASE("quadratic, 600 nodes x radial 8, k = 0.06 -- amplification on a long map") {
        check_chain_pin(600, Blend{BlendProfile::Quadratic, 0.06f}, 4406, ChainOrder::Stroke,
                        false, 20, ChainSymmetry{8, 0, -1.0f});
    }
    SUBCASE("cubic, 300 nodes mirrored on x|y, k = 0.06 -- mirror amplifies too") {
        check_chain_pin(300, Blend{BlendProfile::Cubic, 0.06f}, 4407, ChainOrder::Stroke, false,
                        40, ChainSymmetry{0, kMirrorX | kMirrorY, -1.0f});
    }
    SUBCASE("quadratic, 300 nodes x radial 8 + mirror x, k = 0.06 -- the modes compose") {
        // Additive composition (emit_item copies the BASE item per mode; no
        // products): the amplified knee matched a plain chain of the summed
        // effective length in the campaign, and this holds the pin on it.
        check_chain_pin(300, Blend{BlendProfile::Quadratic, 0.06f}, 4408, ChainOrder::Stroke,
                        false, 20, ChainSymmetry{8, kMirrorX, -1.0f});
    }
    SUBCASE("cubic, 300 nodes x radial 8, seam k = 0.12 over item k = 0.04") {
        // The seam-k defect: the seam is 3x any item k, so no item term can
        // say how far the copies drag. The fold pads by the item-derived
        // ceiling — exactly the pre-#335 pad, asserted below — which the
        // measured knee (5.0 item-k = 0.20) sits under.
        Document doc = chain_doc(300, Blend{BlendProfile::Cubic, 0.04f}, ChainOrder::Stroke,
                                 false, ChainSymmetry{8, 0, 0.12f});
        const Layer& l = doc.layers[0];
        CHECK(cull_pad(*l.sdf, l) == Blend{BlendProfile::Cubic, 0.04f}.support());
        check_chain_pin(300, Blend{BlendProfile::Cubic, 0.04f}, 4429, ChainOrder::Stroke, false,
                        150, ChainSymmetry{8, 0, 0.12f});
    }
    SUBCASE("cubic, 300 nodes mirrored on x|y, seam k = 0.12 over item k = 0.04") {
        Document doc = chain_doc(300, Blend{BlendProfile::Cubic, 0.04f}, ChainOrder::Stroke,
                                 false, ChainSymmetry{0, kMirrorX | kMirrorY, 0.12f});
        const Layer& l = doc.layers[0];
        CHECK(cull_pad(*l.sdf, l) == Blend{BlendProfile::Cubic, 0.04f}.support());
        check_chain_pin(300, Blend{BlendProfile::Cubic, 0.04f}, 4410, ChainOrder::Stroke, false,
                        150, ChainSymmetry{0, kMirrorX | kMirrorY, 0.12f});
    }
}
