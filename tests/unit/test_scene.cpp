#include <doctest/doctest.h>

#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using namespace clay::scene;

using clay_test::gnarly_document;
using clay_test::item;

TEST_CASE("ordered edit list: order matters") {
    Document a;
    Layer& la = a.add_sdf_layer("l");
    la.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    la.sdf->insert(item(Prim::box(cf3(2, 2, 2)), cf3(0, 2, 0), Op::Subtract));

    Document b;
    Layer& lb = b.add_sdf_layer("l");
    lb.sdf->insert(item(Prim::box(cf3(2, 2, 2)), cf3(0, 2, 0), Op::Subtract));
    lb.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));

    Tape ta = compile_document(a);
    Tape tb = compile_document(b);
    // subtract-first has nothing to carve: full sphere remains at (0,0.5,0)
    cfloat3 p = cf3(0, 0.5f, 0);
    CHECK(ta.eval(p).d > 0.0f);   // carved away
    CHECK(tb.eval(p).d < 0.0f);   // still solid
}

TEST_CASE("instance follows source edits") {
    Document doc;
    Layer& src = doc.add_sdf_layer("src");
    src.sdf->insert(item(Prim::sphere(0.5f), cf3(0, 0, 0)));
    Layer* inst = doc.instance_layer(src.id, "inst");
    REQUIRE(inst);
    inst->xform.position = cf3(5, 0, 0);

    Tape t0 = compile_document(doc);
    CHECK(t0.eval(cf3(5, 0.8f, 0)).d > 0.0f);
    // edit the SOURCE content: both instances update
    doc.layers[0].sdf->insert(item(Prim::sphere(0.5f), cf3(0, 1, 0)));
    Tape t1 = compile_document(doc);
    CHECK(t1.eval(cf3(5, 0.8f, 0)).d < 0.0f);  // instance grew too
}

TEST_CASE("tape matches reference tree evaluation (gnarly scene)") {
    Document doc = gnarly_document();
    Tape tape = compile_document(doc);
    REQUIRE(!tape.empty());
    clay_test::Lcg rng(301);
    for (int i = 0; i < 2000; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        CTapeValue tv = tape.eval(p);
        CTapeValue rv = clay_test::ref_eval_document(doc, p);
        CHECK(tv.d == doctest::Approx(rv.d).epsilon(1e-5));
        CHECK(clength(tv.color - rv.color) < 1e-4f);
    }
}

TEST_CASE("invisible items and layers are not compiled") {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    NodeId hidden = l.sdf->insert(item(Prim::sphere(1.0f), cf3(3, 0, 0)));
    l.sdf->find_mut(hidden)->visible = false;
    Tape t = compile_document(doc);
    CHECK(t.eval(cf3(3, 0, 0)).d > 0.5f);  // hidden sphere absent
    CHECK(t.eval(cf3(0, 0, 0)).d < 0.0f);
}

TEST_CASE("parameter edit rewrites params only, not instruction count") {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    NodeId id = l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    l.sdf->insert(item(Prim::box(cf3(1, 1, 1)), cf3(2, 0, 0), Op::Add,
                       Blend{BlendProfile::Quadratic, 0.1f}));
    Tape t0 = compile_document(doc);
    l.sdf->find_mut(id)->prim = Prim::sphere(1.5f);
    Tape t1 = compile_document(doc);
    REQUIRE(t0.instrs.size() == t1.instrs.size());
    REQUIRE(t0.params.size() == t1.params.size());
    for (std::size_t i = 0; i < t0.instrs.size(); ++i) {
        CHECK(t0.instrs[i].op == t1.instrs[i].op);
        CHECK(t0.instrs[i].param_offset == t1.instrs[i].param_offset);
    }
}

TEST_CASE("influence bounds are conservative (band-clamped bit-identity outside)") {
    // Raw far-field values legitimately shift when a smooth-blend operand is
    // removed (smin deviates wherever |a-b| < support, arbitrarily far from
    // both surfaces). The guarantee the bound provides — and what bricks
    // store — is band-clamped identity: outside bound ⊕ band, clamp(d,±band)
    // is bit-identical with and without the item.
    Document doc = gnarly_document();
    Tape full = compile_document(doc);
    const float band = 0.2f;
    Layer& layer = doc.layers[0];
    std::vector<NodeId> roots = layer.sdf->roots;
    clay_test::Lcg rng(302);
    for (NodeId id : roots) {
        const Node* n = layer.sdf->find(id);
        if (!n || n->is_group) continue;
        math::Aabb bound = item_influence_bound(*n, layer).dilated(band);
        Node saved = *n;
        NodeId parent;
        int index;
        layer.sdf->locate(id, &parent, &index);
        layer.sdf->remove(id);
        Tape without = compile_document(doc);
        int outside_pts = 0;
        for (int i = 0; i < 800; ++i) {
            cfloat3 p = rng.vec3(-5, 5);
            if (bound.contains(p)) continue;
            // instanced layer shares content: skip its copy of the bound too
            math::Aabb inst_bound = item_influence_bound(saved, doc.layers[2]).dilated(band);
            if (inst_bound.contains(p)) continue;
            ++outside_pts;
            float df = cclamp(full.eval(p).d, -band, band);
            float dw = cclamp(without.eval(p).d, -band, band);
            CHECK(df == dw);  // exact equality
        }
        CHECK(outside_pts > 100);
        layer.sdf->reinsert({saved}, parent, index);
    }
}

TEST_CASE("intersect items report infinite influence") {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    NodeId id = l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0), Op::Intersect));
    CHECK(item_influence_bound(*l.sdf->find(id), l).is_infinite());
}

TEST_CASE("per-brick culled tape: band-clamped bit-identity") {
    Document doc = gnarly_document();
    Tape full = compile_document(doc);
    float band = 0.15f;  // ±3 voxels at a plausible resolution
    clay_test::Lcg rng(303);
    // brick regions across the scene, including far-empty ones
    for (int b = 0; b < 40; ++b) {
        cfloat3 corner = rng.vec3(-4, 4);
        math::Aabb brick{corner, corner + cf3(0.4f, 0.4f, 0.4f)};
        CullRegion cull{brick.dilated(band)};
        Tape culled = compile_document(doc, &cull);
        CHECK(culled.instrs.size() <= full.instrs.size());
        for (int i = 0; i < 200; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            float df = cclamp(full.eval(p).d, -band, band);
            float dc = cclamp(culled.eval(p).d, -band, band);
            CHECK(df == dc);  // exact equality, not approx
        }
    }
}

TEST_CASE("culling drops instructions for far bricks") {
    Document doc = gnarly_document();
    Tape full = compile_document(doc);
    CullRegion far_cull{math::Aabb{cf3(50, 50, 50), cf3(51, 51, 51)}};
    Tape culled = compile_document(doc, &far_cull);
    CHECK(culled.instrs.size() < full.instrs.size());
    CHECK(culled.empty());
}

TEST_CASE("tape tracks exactness: ellipsoid or blends downgrade") {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    Tape exact = compile_document(doc);
    CHECK(exact.info.is_exact);
    CHECK(exact.safe_step_scale() == doctest::Approx(1.0f));

    l.sdf->insert(item(Prim::box(cf3(1, 1, 1)), cf3(2, 0, 0), Op::Add,
                       Blend{BlendProfile::Quadratic, 0.2f}));
    Tape blended = compile_document(doc);
    CHECK_FALSE(blended.info.is_exact);
    CHECK(blended.safe_step_scale() == doctest::Approx(1.0f));  // still safe at full step
}
