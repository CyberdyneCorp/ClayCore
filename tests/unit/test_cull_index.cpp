#include <doctest/doctest.h>

#include "clay/field/volume.h"
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
    CHECK(index.feather_pad() == 0.0f);  // no feathered replace in this corpus
}

TEST_CASE("cull index: byte-identical per-brick tapes on the gnarly corpus") {
    // Mirror + mirrored items, nested groups 4 deep with blends, a stroke,
    // paint, layer transforms and an instanced layer (shared SdfContent
    // compiled under two layers — the case that keys the index per layer).
    check_document(gnarly_document(), 401);
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
    CHECK(index.feather_pad() == feather_cull_pad(*lb.sdf, lb));
    CHECK(index.feather_pad() > 0.0f);

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
