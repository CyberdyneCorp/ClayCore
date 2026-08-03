#include <doctest/doctest.h>

#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using namespace clay::scene;

using clay_test::item;
using clay_test::Lcg;

namespace {

constexpr float kSqrtHalf = 0.70710678f;

// Band-clamped rigidity: wherever the item field b exceeds the mode's
// support plus the band, the combined result clamps identically to a.
template <typename F>
void check_band_rigidity(F op, float support, std::uint64_t seed) {
    const float band = 0.25f;
    Lcg rng(seed);
    for (int i = 0; i < 4000; ++i) {
        float a = rng.range(-6.0f, 6.0f);
        float b = support + band + rng.range(1e-3f, 6.0f);
        float r = op(a, b);
        CHECK(cclamp(r, -band, band) == cclamp(a, -band, band));
    }
}

// Base sphere + one extended item; identity layer transform so surface
// points are exact.
Document base_plus(Node extra) {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    l.sdf->insert(extra);
    return doc;
}

// A scene exercising every extended mode on top of a base sphere, with a
// non-trivial layer transform and an extended-op group. The items sit on
// well-separated directions of the base sphere: depth-limited carves read
// the accumulated field up to their depth below the band, so stacking one
// extended item's interaction zone onto another's would (legitimately)
// leak raw deep-field differences past the per-item bounds — the same soft
// spot wide smooth-subtracts have.
Document extended_document(bool include_group = true) {
    Document doc;
    Layer& l = doc.add_sdf_layer("ext");
    l.xform.position = cf3(0.15f, -0.1f, 0.2f);
    l.xform.rotation = math::Quat::from_axis_angle(cf3(0, 0, 1), 0.3f);
    l.xform.scale = 1.1f;
    SdfContent& c = *l.sdf;

    c.insert(item(Prim::sphere(1.2f), cf3(0, 0, 0)));

    Node groove = item(Prim::box(cf3(0.2f, 0.3f, 0.3f)), cf3(1.15f, 0, 0), Op::Groove,
                       Blend{BlendProfile::Hard, 0.15f});
    groove.rounding = 0.1f;
    groove.xform.scale = 1.2f;  // exercises item scale in the rb mapping
    c.insert(groove);

    Node tongue = item(Prim::box(cf3(0.2f, 0.3f, 0.3f)), cf3(-1.15f, 0, 0), Op::Tongue,
                       Blend{BlendProfile::Hard, 0.14f});
    tongue.rounding = 0.08f;
    c.insert(tongue);

    c.insert(item(Prim::sphere(0.5f), cf3(0, 1.25f, 0), Op::Pipe,
                  Blend{BlendProfile::Hard, 0.12f}));
    c.insert(item(Prim::sphere(0.5f), cf3(0, -1.25f, 0), Op::Engrave,
                  Blend{BlendProfile::Hard, 0.1f}));
    c.insert(item(Prim::sphere(0.5f), cf3(0, 0, 1.25f), Op::Emboss,
                  Blend{BlendProfile::Hard, 0.1f}));
    c.insert(item(Prim::box(cf3(0.3f, 0.3f, 0.4f)), cf3(0, 0, -1.2f), Op::Inset,
                  Blend{BlendProfile::Hard, 0.12f}));
    c.insert(item(Prim::sphere(0.4f), cf3(1.0f, 1.0f, 1.0f), Op::Shell,
                  Blend{BlendProfile::Hard, 0.08f}));
    c.insert(item(Prim::sphere(0.35f), cf3(0.8f, -0.8f, 0.8f), Op::Replace));

    if (include_group) {
        Node g;
        g.is_group = true;
        g.op = Op::Engrave;
        g.blend = Blend{BlendProfile::Hard, 0.08f};
        NodeId gid = c.insert(g);
        c.insert(item(Prim::sphere(0.4f), cf3(-0.85f, 0.85f, -0.55f)), gid);
        c.insert(item(Prim::sphere(0.4f), cf3(-0.55f, 0.85f, -0.85f)), gid);
    }
    return doc;
}

}  // namespace

// -- kernel-level properties --------------------------------------------------

TEST_CASE("extended blends: band-clamped identity outside the documented support") {
    const float k = 0.3f;
    const float rb = 0.2f;
    CHECK(ccombine_extended_support(ccombine_groove, k, rb) == rb);
    CHECK(ccombine_extended_support(ccombine_tongue, k, rb) == rb);
    CHECK(ccombine_extended_support(ccombine_pipe, k, rb) == k);
    CHECK(ccombine_extended_support(ccombine_engrave, k, rb) == k);
    CHECK(ccombine_extended_support(ccombine_emboss, k, rb) == k);
    CHECK(ccombine_extended_support(ccombine_inset, k, rb) == 0.0f);
    CHECK(ccombine_extended_support(ccombine_shell, k, rb) == k);
    CHECK(ccombine_extended_support(ccombine_replace, k, rb) == 0.0f);

    check_band_rigidity([&](float a, float b) { return op_groove(a, b, k, rb); }, rb, 401);
    check_band_rigidity([&](float a, float b) { return op_tongue(a, b, k, rb); }, rb, 402);
    check_band_rigidity([&](float a, float b) { return op_pipe(a, b, k); }, k, 403);
    check_band_rigidity([&](float a, float b) { return op_engrave(a, b, k); }, k, 404);
    check_band_rigidity([&](float a, float b) { return op_emboss(a, b, k); }, k, 405);
    check_band_rigidity([&](float a, float b) { return op_inset(a, b, k); }, 0.0f, 406);
    check_band_rigidity([&](float a, float b) { return op_shell_union(a, b, k); }, k, 407);
    check_band_rigidity([&](float a, float b) { return op_replace(a, b); }, 0.0f, 408);
}

TEST_CASE("extended blends: geometric behavior at exact configurations") {
    const float r = 0.3f;
    SUBCASE("pipe: solid tube of radius r on the intersection curve") {
        CHECK(op_pipe(0.0f, 0.0f, r) == doctest::Approx(-r));
        CHECK(op_pipe(5.0f, 4.0f, r) == doctest::Approx(5.0f));  // min keeps a far away
    }
    SUBCASE("engrave carves a V of depth r where b's surface crosses a's") {
        CHECK(op_engrave(0.0f, 0.0f, r) == doctest::Approx(r * kSqrtHalf));  // carved
        CHECK(op_engrave(-r, 0.0f, r) == doctest::Approx(0.0f));  // groove bottom at depth r
        CHECK(op_engrave(-2.0f, 0.0f, r) < 0.0f);                 // deep interior stays solid
        CHECK(op_engrave(0.4f, 2.0f, r) == doctest::Approx(0.4f));
    }
    SUBCASE("emboss raises a V ridge of height r (adds material, never removes)") {
        CHECK(op_emboss(0.0f, 0.0f, r) == doctest::Approx(-r * kSqrtHalf));  // added
        CHECK(op_emboss(r, 0.0f, r) == doctest::Approx(0.0f));  // ridge apex at height r
        CHECK(op_emboss(0.4f, 2.0f, r) == doctest::Approx(0.4f));
        Lcg rng(409);
        for (int i = 0; i < 1000; ++i) {
            float a = rng.range(-3.0f, 3.0f);
            float b = rng.range(-3.0f, 3.0f);
            CHECK(op_emboss(a, b, r) <= a);  // only ever adds
        }
    }
    SUBCASE("groove carves a flat-bottomed channel (depth ra, half-width rb)") {
        const float ra = 0.25f, rb = 0.15f;
        CHECK(op_groove(0.0f, 0.0f, ra, rb) == doctest::Approx(cmin(ra, rb)));  // carved
        CHECK(op_groove(-ra, 0.0f, ra, rb) == doctest::Approx(0.0f));  // channel floor
        CHECK(op_groove(-1.0f, 0.0f, ra, rb) == doctest::Approx(-1.0f + ra));  // solid below
        CHECK(op_groove(0.0f, rb + 0.5f, ra, rb) == doctest::Approx(0.0f));  // away from b
    }
    SUBCASE("tongue raises a flat-topped ridge (height ra, half-width rb)") {
        const float ra = 0.25f, rb = 0.15f;
        CHECK(op_tongue(0.1f, 0.0f, ra, rb) == doctest::Approx(-cmin(ra - 0.1f, rb)));
        CHECK(op_tongue(ra, 0.0f, ra, rb) == doctest::Approx(0.0f));  // ridge top
        CHECK(op_tongue(0.0f, rb + 0.5f, ra, rb) == doctest::Approx(0.0f));  // away from b
    }
    SUBCASE("inset carves a recessed panel of depth r over b's interior") {
        CHECK(op_inset(0.0f, -0.5f, r) == doctest::Approx(r));    // recessed on a's surface
        CHECK(op_inset(-r, -5.0f, r) == doctest::Approx(0.0f));   // panel floor at depth r
        CHECK(op_inset(0.05f, 0.5f, r) == doctest::Approx(0.05f));  // outside b: untouched
    }
    SUBCASE("shell unions the onion of b (wall half-thickness t)") {
        const float t = 0.08f;
        CHECK(op_shell_union(10.0f, 0.0f, t) == doctest::Approx(-t));      // wall is solid
        CHECK(op_shell_union(10.0f, -0.5f, t) == doctest::Approx(0.42f));  // interior hollow
        CHECK(op_shell_union(-0.5f, -0.5f, t) == doctest::Approx(-0.5f));  // a wins inside a
    }
    SUBCASE("replace: b's field replaces a inside b") {
        CHECK(op_replace(-2.0f, -0.4f) == doctest::Approx(-0.4f));
        CHECK(op_replace(0.3f, -0.4f) == doctest::Approx(-0.4f));
        CHECK(op_replace(5.0f, -0.4f) == doctest::Approx(-0.4f));
        CHECK(op_replace(2.0f, 3.0f) == doctest::Approx(2.0f));   // a kept outside b
        CHECK(op_replace(-2.0f, 0.5f) == doctest::Approx(-0.5f));  // b's boundary carves a
    }
}

// -- scene-level behavior ------------------------------------------------------

TEST_CASE("extended ops: tape matches reference tree evaluation") {
    Document doc = extended_document();
    Tape tape = compile_document(doc);
    REQUIRE(!tape.empty());
    Lcg rng(411);
    for (int i = 0; i < 2000; ++i) {
        cfloat3 p = rng.vec3(-4, 4);
        CTapeValue tv = tape.eval(p);
        CTapeValue rv = clay_test::ref_eval_document(doc, p);
        CHECK(tv.d == doctest::Approx(rv.d).epsilon(1e-5));
        CHECK(clength(tv.color - rv.color) < 1e-4f);
    }
}

TEST_CASE("extended ops: geometry and color on real scenes") {
    // Intersection circle of |p| = 1 and |p - (1.2,0,0)| = 0.5: x = 0.9125.
    const cfloat3 xing = cf3(0.9125f, 0.4090538f, 0.0f);
    const cfloat3 bcol = cf3(0.9f, 0.1f, 0.2f);
    const cfloat3 acol = cf3(0.7f, 0.7f, 0.7f);

    SUBCASE("pipe lays a solid tube along the intersection curve, b-colored") {
        Node n = item(Prim::sphere(0.5f), cf3(1.2f, 0, 0), Op::Pipe,
                      Blend{BlendProfile::Hard, 0.12f});
        n.color = bcol;
        Tape t = compile_document(base_plus(n));
        CTapeValue v = t.eval(xing);
        CHECK(v.d == doctest::Approx(-0.12f).epsilon(1e-3));
        CHECK(clength(v.color - bcol) < 1e-4f);
    }
    SUBCASE("engrave carves where b's surface crosses a's, keeping a's color") {
        Node n = item(Prim::sphere(0.5f), cf3(1.2f, 0, 0), Op::Engrave,
                      Blend{BlendProfile::Hard, 0.1f});
        n.color = bcol;
        Tape t = compile_document(base_plus(n));
        CTapeValue v = t.eval(xing);
        CHECK(v.d == doctest::Approx(0.1f * kSqrtHalf).epsilon(1e-3));
        CHECK(clength(v.color - acol) < 1e-4f);
    }
    SUBCASE("emboss raises a ridge where b's surface crosses a's, b-colored") {
        Node n = item(Prim::sphere(0.5f), cf3(1.2f, 0, 0), Op::Emboss,
                      Blend{BlendProfile::Hard, 0.1f});
        n.color = bcol;
        Tape t = compile_document(base_plus(n));
        CTapeValue v = t.eval(xing);
        CHECK(v.d == doctest::Approx(-0.1f * kSqrtHalf).epsilon(1e-3));
        CHECK(clength(v.color - bcol) < 1e-4f);
    }
    SUBCASE("groove carves along b's rounded surface; field increases on a") {
        // rounding 0.1 puts b's surface (and the channel center) at raw
        // distance 0.1 from the sphere: radius 0.6 around (1.2,0,0).
        Node n = item(Prim::sphere(0.5f), cf3(1.2f, 0, 0), Op::Groove,
                      Blend{BlendProfile::Hard, 0.2f});
        n.rounding = 0.1f;
        Tape with = compile_document(base_plus(n));
        Tape without = compile_document(base_plus(item(Prim::sphere(1.0f), cf3(9, 9, 9))));
        // Intersection of |p| = 1 and |p - (1.2,0,0)| = 0.6: x = 0.8666667.
        cfloat3 p = cf3(0.8666667f, 0.4988878f, 0.0f);
        CHECK(without.eval(p).d == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(with.eval(p).d == doctest::Approx(0.1f).epsilon(1e-3));  // min(ra, rb) = rb
    }
    SUBCASE("tongue adds a ridge along b's rounded surface, b-colored") {
        Node n = item(Prim::sphere(0.5f), cf3(1.2f, 0, 0), Op::Tongue,
                      Blend{BlendProfile::Hard, 0.2f});
        n.rounding = 0.1f;
        n.color = bcol;
        Tape t = compile_document(base_plus(n));
        cfloat3 p = cf3(0.8666667f, 0.4988878f, 0.0f);
        CTapeValue v = t.eval(p);
        CHECK(v.d == doctest::Approx(-0.1f).epsilon(1e-3));  // -min(ra, rb)
        CHECK(clength(v.color - bcol) < 1e-4f);
    }
    SUBCASE("inset recesses a panel over b's interior") {
        Node n = item(Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(0.95f, 0, 0), Op::Inset,
                      Blend{BlendProfile::Hard, 0.15f});
        Tape t = compile_document(base_plus(n));
        CHECK(t.eval(cf3(1.0f, 0, 0)).d == doctest::Approx(0.15f).epsilon(1e-3));
        CHECK(t.eval(cf3(0.85f, 0, 0)).d == doctest::Approx(0.0f).epsilon(1e-3));  // floor
    }
    SUBCASE("shell unions a hollow shell of b, b-colored on the wall") {
        Node n = item(Prim::sphere(0.5f), cf3(2.5f, 0, 0), Op::Shell,
                      Blend{BlendProfile::Hard, 0.08f});
        n.color = bcol;
        Tape t = compile_document(base_plus(n));
        CHECK(t.eval(cf3(2.5f, 0, 0)).d == doctest::Approx(0.42f).epsilon(1e-3));  // hollow
        CTapeValue wall = t.eval(cf3(2.0f, 0, 0));
        CHECK(wall.d == doctest::Approx(-0.08f).epsilon(1e-3));
        CHECK(clength(wall.color - bcol) < 1e-4f);
    }
    SUBCASE("replace: field inside b equals b, b-colored") {
        Node n = item(Prim::sphere(0.4f), cf3(0.9f, 0, 0), Op::Replace);
        n.color = bcol;
        Tape t = compile_document(base_plus(n));
        CTapeValue v = t.eval(cf3(0.9f, 0, 0));
        CHECK(v.d == doctest::Approx(-0.4f).epsilon(1e-3));
        CHECK(clength(v.color - bcol) < 1e-4f);
        // outside b, a's field and color survive
        CTapeValue away = t.eval(cf3(-0.5f, 0, 0));
        CHECK(away.d == doctest::Approx(-0.5f).epsilon(1e-3));
        CHECK(clength(away.color - acol) < 1e-4f);
    }
    SUBCASE("material-creating modes seed an empty chain instead of vanishing") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node shell = item(Prim::sphere(0.5f), cf3(0, 0, 0), Op::Shell,
                          Blend{BlendProfile::Hard, 0.08f});
        l.sdf->insert(shell);
        Tape t = compile_document(doc);
        REQUIRE(!t.empty());
        CHECK(t.eval(cf3(0.5f, 0, 0)).d == doctest::Approx(-0.08f).epsilon(1e-3));
        CHECK(t.eval(cf3(0, 0, 0)).d == doctest::Approx(0.42f).epsilon(1e-3));

        // carving modes with nothing beneath still produce nothing
        Document doc2;
        Layer& l2 = doc2.add_sdf_layer("l");
        l2.sdf->insert(item(Prim::sphere(0.5f), cf3(0, 0, 0), Op::Groove,
                            Blend{BlendProfile::Hard, 0.2f}));
        CHECK(compile_document(doc2).empty());
    }
}

TEST_CASE("extended ops: influence bounds are conservative (band-clamped)") {
    Document doc = extended_document();
    Tape full = compile_document(doc);
    const float band = 0.2f;
    Layer& layer = doc.layers[0];
    std::vector<NodeId> roots = layer.sdf->roots;
    Lcg rng(412);
    for (NodeId id : roots) {
        const Node* n = layer.sdf->find(id);
        if (!n || n->is_group || !op_is_extended(n->op)) continue;
        math::Aabb bound = item_influence_bound(*n, layer).dilated(band);
        CHECK_FALSE(bound.is_infinite());  // none of these are intersect-like
        Node saved = *n;
        NodeId parent = kNoNode;
        int index = -1;
        layer.sdf->locate(id, &parent, &index);
        layer.sdf->remove(id);
        Tape without = compile_document(doc);
        int outside_pts = 0;
        for (int i = 0; i < 800; ++i) {
            cfloat3 p = rng.vec3(-5, 5);
            if (bound.contains(p)) continue;
            ++outside_pts;
            float df = cclamp(full.eval(p).d, -band, band);
            float dw = cclamp(without.eval(p).d, -band, band);
            CHECK(df == dw);  // exact equality
        }
        CHECK(outside_pts > 100);
        layer.sdf->reinsert({saved}, parent, index);
    }
}

TEST_CASE("extended ops: group influence bound is conservative (band-clamped)") {
    Document with_g = extended_document(true);
    Document without_g = extended_document(false);
    const float band = 0.2f;
    Layer& layer = with_g.layers[0];
    NodeId gid = kNoNode;
    for (NodeId id : layer.sdf->roots) {
        const Node* n = layer.sdf->find(id);
        if (n && n->is_group) gid = id;
    }
    REQUIRE(gid != kNoNode);
    math::Aabb bound = node_influence_bound(*layer.sdf, gid, layer).dilated(band);
    Tape full = compile_document(with_g);
    Tape reduced = compile_document(without_g);
    Lcg rng(413);
    int outside_pts = 0;
    for (int i = 0; i < 1200; ++i) {
        cfloat3 p = rng.vec3(-5, 5);
        if (bound.contains(p)) continue;
        ++outside_pts;
        float df = cclamp(full.eval(p).d, -band, band);
        float dr = cclamp(reduced.eval(p).d, -band, band);
        CHECK(df == dr);
    }
    CHECK(outside_pts > 200);
}

TEST_CASE("extended ops: per-brick culled tape is band-clamp identical") {
    Document doc = extended_document();
    Tape full = compile_document(doc);
    float band = 0.15f;
    Lcg rng(414);
    for (int b = 0; b < 40; ++b) {
        cfloat3 corner = rng.vec3(-4, 4);
        math::Aabb brick{corner, corner + cf3(0.4f, 0.4f, 0.4f)};
        CullRegion cull{brick.dilated(band)};
        Tape culled = compile_document(doc, &cull);
        for (int i = 0; i < 200; ++i) {
            cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                            rng.range(brick.min.y, brick.max.y),
                            rng.range(brick.min.z, brick.max.z));
            float df = cclamp(full.eval(p).d, -band, band);
            float dc = cclamp(culled.eval(p).d, -band, band);
            CHECK(df == dc);  // exact equality, not approx
        }
    }
    // far-empty region culls everything
    CullRegion far_cull{math::Aabb{cf3(50, 50, 50), cf3(51, 51, 51)}};
    CHECK(compile_document(doc, &far_cull).empty());
}

TEST_CASE("extended ops: diagonal modes lower the safe step, and it holds") {
    Document doc = extended_document();
    Tape tape = compile_document(doc);
    CHECK_FALSE(tape.info.is_exact);
    // pipe/engrave/emboss can steepen the gradient to sqrt(2) each
    CHECK(tape.safe_step_scale() <= 0.70711f);
    clay_test::check_conservative_steps(
        [&](cfloat3 p) { return tape.eval(p).d; }, tape.safe_step_scale(), 3.0f, 1500, 415);
}
