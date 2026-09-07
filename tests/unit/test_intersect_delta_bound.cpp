#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// THE PROOF OBLIGATION BEHIND #471's FAST PATH, run as a search for a
// counterexample rather than as a demonstration.
//
// `command_surface_delta_bound` claims something the influence bound does not:
// that moving an INTERSECT operand cannot change the MESHING-RELEVANT
// classification of any point outside the swept union of where the operand was
// and where it went. The influence bound stays what it was -- an intersect
// really can change the field anywhere the layer has material -- and this file
// is about the narrower claim, which is the only one the refill region rests
// on.
//
// WHAT IS CHECKED, at every sampled point outside the claimed box (dilated by
// the band, because every consumer of a dirty region dilates by the band:
// BrickCache::mark_dirty does, and the seed store dilates each brick by band +
// pad):
//
//     sign(before)      != sign(after)
//     |before| >  band  && |after| <= band
//     |before| <= band  && |after| >  band
//
// Any of the three is a counterexample and fails the fixture. A counterexample
// is a reason to WIDEN THE BOUND OR NARROW THE FAST PATH -- never to loosen
// this file.
//
// EACH FIXTURE ALSO DECLARES WHETHER THE FAST PATH MAY FIRE AT ALL, and that
// half matters as much: a probe that passes because the bound was refused
// everywhere proves nothing about the bound. The fixtures that must fall back
// (an infinite repeat, an unbounded primitive, a spatial morph, a deformed or
// gated operand) assert the refusal directly.

using namespace clay;
using clay_test::item;
using kernel::cf3;
using kernel::cfloat3;
using math::Aabb;
using scene::Blend;
using scene::BlendProfile;
using scene::Document;
using scene::Layer;
using scene::LayerId;
using scene::Node;
using scene::NodeId;
using scene::Op;
using scene::Prim;
using scene::SdfContent;

namespace {

// The band every bound in this engine is stated against, and the one the
// brick cache's default configuration lands near.
constexpr float kBand = 0.15f;

// One scene plus the move to make in it.
struct Fixture {
    Document doc;
    LayerId layer = 0;
    NodeId cutter = 0;
    math::Transform after;
    cfloat3 after_axes = cf3(1, 1, 1);
    // Whether the delta path is expected to answer. False is a fixture whose
    // point is the REFUSAL.
    bool provable = true;
};

// A body worth cutting: four overlapping lumps about the origin, spanning
// roughly [-1.4, 1.4]. Non-uniform on purpose -- a fixture whose form is one
// centred sphere hides every mistake that depends on where the operand sits
// relative to the material.
void add_body(SdfContent& c, Blend blend = {}) {
    c.insert(item(Prim::sphere(0.8f), cf3(0, 0, 0), Op::Add, blend));
    c.insert(item(Prim::sphere(0.5f), cf3(0.7f, 0.3f, 0), Op::Add, blend));
    c.insert(item(Prim::box(cf3(0.35f, 0.6f, 0.4f)), cf3(-0.7f, -0.2f, 0.1f), Op::Add, blend));
    c.insert(item(Prim::torus(0.6f, 0.18f), cf3(0, 0.8f, 0), Op::Add, blend));
}

math::Transform at(cfloat3 p, float scale = 1.0f) {
    math::Transform t;
    t.position = p;
    t.scale = scale;
    return t;
}

math::Transform turned(cfloat3 p, cfloat3 axis, float angle, float scale = 1.0f) {
    math::Transform t = at(p, scale);
    t.rotation = math::Quat::from_axis_angle(axis, angle);
    return t;
}

// The move, as the command layer sees it.
scene::Command move_cmd(const Fixture& f) {
    return scene::Command{scene::SetTransformCmd{f.layer, f.cutter, f.after, f.after_axes}};
}

// The meshing-relevant classification of one sample.
struct Class {
    int sign;    // -1 inside, +1 outside (0 counted as outside, as the mesher does)
    bool in_band;
};

Class classify(float d) { return Class{d < 0.0f ? -1 : 1, std::fabs(d) <= kBand}; }

struct Verdict {
    bool proven = false;
    Aabb box;
    Aabb conservative;  // what the influence path would have dirtied
    long outside = 0;   // samples actually tested
    long sign_flips = 0;
    long band_entered = 0;
    long band_left = 0;
    float worst = 0.0f;  // largest |before - after| among the violations
    cfloat3 worst_at = cf3(0, 0, 0);
    std::string worst_what;
};

// Sample points: a lattice over a region comfortably larger than anything
// these fixtures occupy, plus a dense random SHELL just outside the claimed
// box -- where a bound that is one term short shows first, and where a plain
// lattice puts almost no samples.
std::vector<cfloat3> samples(const Aabb& box, float extent, std::uint64_t seed) {
    std::vector<cfloat3> pts;
    const int side = 25;
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < side; ++k) {
                auto f = [&](int n) {
                    return static_cast<float>(n) / static_cast<float>(side - 1) * 2 * extent -
                           extent;
                };
                pts.push_back(cf3(f(i), f(j), f(k)));
            }
    if (!box.empty() && !box.is_infinite()) {
        const Aabb inner = box.dilated(kBand);
        const Aabb outer = box.dilated(kBand * 4.0f);
        clay_test::Lcg rng(seed);
        for (int i = 0; i < 12000; ++i) {
            const cfloat3 p = cf3(rng.range(outer.min.x, outer.max.x),
                                  rng.range(outer.min.y, outer.max.y),
                                  rng.range(outer.min.z, outer.max.z));
            if (!inner.contains(p)) pts.push_back(p);
        }
    }
    return pts;
}

// Evaluate before, move, evaluate after, and compare everything outside the
// claimed box. The tape is the production evaluator on purpose: what is under
// test here is the BOUND, and an expectation computed by a second copy of the
// bound's own arithmetic would prove nothing.
Verdict probe(Fixture f, float extent = 4.0f, std::uint64_t seed = 0x471u) {
    Verdict v;
    const scene::Command cmd = move_cmd(f);

    scene::LayerExtent memo;
    v.conservative = scene::command_influence_bound(f.doc, cmd, &memo);
    const std::optional<Aabb> before_delta = scene::command_surface_delta_bound(f.doc, cmd);
    const scene::Tape before_tape = scene::compile_document(f.doc);

    REQUIRE(scene::apply(f.doc, cmd).has_value());

    const std::optional<Aabb> after_delta = scene::command_surface_delta_bound(f.doc, cmd);
    {
        scene::LayerExtent after_memo;
        v.conservative.expand(scene::command_influence_bound(f.doc, cmd, &after_memo));
    }
    const scene::Tape after_tape = scene::compile_document(f.doc);

    v.proven = before_delta.has_value() && after_delta.has_value();
    if (!v.proven) return v;
    v.box = *before_delta;
    v.box.expand(*after_delta);

    const Aabb outside_of = v.box.dilated(kBand);
    for (const cfloat3& p : samples(v.box, extent, seed)) {
        if (outside_of.contains(p)) continue;
        ++v.outside;
        const float b = before_tape.eval(p).d;
        const float a = after_tape.eval(p).d;
        const Class cb = classify(b), ca = classify(a);
        const char* what = nullptr;
        if (cb.sign != ca.sign) {
            ++v.sign_flips;
            what = "sign";
        } else if (!cb.in_band && ca.in_band) {
            ++v.band_entered;
            what = "entered the band";
        } else if (cb.in_band && !ca.in_band) {
            ++v.band_left;
            what = "left the band";
        }
        if (what && std::fabs(b - a) >= v.worst) {
            v.worst = std::fabs(b - a);
            v.worst_at = p;
            v.worst_what = what;
        }
    }
    return v;
}

// One fixture, run and judged.
void check(const char* name, Fixture f, float extent = 4.0f) {
    const bool wanted = f.provable;
    const Verdict v = probe(std::move(f), extent);
    const std::string at = " worst |db| " + std::to_string(v.worst) + " at (" +
                           std::to_string(v.worst_at.x) + ", " + std::to_string(v.worst_at.y) +
                           ", " + std::to_string(v.worst_at.z) + ")";
    const std::string who = std::string("fixture \"") + name + "\": ";
    const std::string wrong_side =
        who + (wanted ? "the delta path refused a case it must prove"
                      : "the delta path answered a case it must refuse");
    CHECK_MESSAGE(v.proven == wanted, wrong_side);
    if (!v.proven) return;
    // A fixture whose samples all land inside the box tests nothing.
    const std::string thin =
        who + "only " + std::to_string(v.outside) + " samples outside the bound";
    CHECK_MESSAGE(v.outside > 1000, thin);
    const std::string flips =
        who + std::to_string(v.sign_flips) + " sign changes outside the bound," + at;
    CHECK_MESSAGE(v.sign_flips == 0, flips);
    const std::string entered =
        who + std::to_string(v.band_entered) + " samples entered the band outside the bound," + at;
    CHECK_MESSAGE(v.band_entered == 0, entered);
    const std::string left =
        who + std::to_string(v.band_left) + " samples left the band outside the bound," + at;
    CHECK_MESSAGE(v.band_left == 0, left);
}

// -- fixture builders -------------------------------------------------------

// The base case every other one is a variation of: one layer, a body, and an
// intersect operand at the root of it, dragged across the form.
Fixture base(Prim cutter, Blend cut_blend, cfloat3 from, cfloat3 to) {
    Fixture f;
    Layer& l = f.doc.add_sdf_layer("body");
    f.layer = l.id;
    add_body(*l.sdf);
    f.cutter = l.sdf->insert(item(cutter, from, Op::Intersect, cut_blend));
    f.after = at(to);
    return f;
}

Blend hard() { return Blend{}; }
Blend smooth(float k) { return Blend{BlendProfile::Quadratic, k}; }

}  // namespace

TEST_CASE("intersect delta: primitives, hard and smooth") {
    // The four shapes the issue's matrix names, each dragged across the form
    // and each with a hard and a smooth intersect.
    struct Case {
        const char* name;
        Prim prim;
    };
    const Case cases[] = {
        {"sphere", Prim::sphere(0.55f)},
        {"box", Prim::box(cf3(0.4f, 0.5f, 0.45f))},
        {"cylinder", Prim::capped_cylinder(0.25f, 0.8f)},
        {"torus", Prim::torus(0.5f, 0.2f)},
    };
    for (const Case& c : cases) {
        check((std::string(c.name) + " hard").c_str(),
              base(c.prim, hard(), cf3(-0.6f, 0.2f, 0), cf3(0.5f, 0.2f, 0)));
        check((std::string(c.name) + " smooth").c_str(),
              base(c.prim, smooth(0.12f), cf3(-0.6f, 0.2f, 0), cf3(0.5f, 0.2f, 0)));
    }
}

TEST_CASE("intersect delta: rotation and scale, not only translation") {
    {
        Fixture f = base(Prim::box(cf3(0.5f, 0.7f, 0.3f)), smooth(0.1f), cf3(0.1f, 0, 0),
                         cf3(0.1f, 0, 0));
        f.after = turned(cf3(0.1f, 0, 0), cf3(0.3f, 1.0f, 0.2f), 0.9f);
        check("rotated in place", std::move(f));
    }
    {
        Fixture f = base(Prim::sphere(0.5f), hard(), cf3(0, 0.1f, 0), cf3(0, 0.1f, 0));
        f.after = at(cf3(0, 0.1f, 0), 2.1f);
        check("uniformly scaled up", std::move(f));
    }
    {
        Fixture f = base(Prim::sphere(0.9f), smooth(0.08f), cf3(0, 0.1f, 0), cf3(0, 0.1f, 0));
        f.after = at(cf3(0, 0.1f, 0), 0.4f);
        check("uniformly scaled down", std::move(f));
    }
    {
        // The per-axis scale rides the same command (#320), so it is the same
        // edit kind and must take the same path.
        Fixture f = base(Prim::sphere(0.6f), hard(), cf3(0, 0.1f, 0), cf3(0.2f, 0.1f, 0));
        f.after_axes = cf3(2.2f, 0.5f, 1.4f);
        check("squashed per axis", std::move(f));
    }
    {
        Fixture f = base(Prim::capped_cylinder(0.3f, 0.9f), smooth(0.15f), cf3(-0.5f, 0, 0),
                         cf3(0.45f, 0.3f, 0.2f));
        f.after = turned(cf3(0.45f, 0.3f, 0.2f), cf3(1, 0.2f, 0), 1.4f, 1.6f);
        check("moved, turned and scaled at once", std::move(f));
    }
}

TEST_CASE("intersect delta: where the operand sits and where it goes") {
    // The placements the matrix asks for. Each is a different relationship
    // between the operand and the material, and the last one is the case an
    // AABB-swept bound is most likely to get wrong: the operand crosses the
    // whole object in ONE command, so the two ends of the sweep are far apart
    // and everything between them is inside the box.
    check("cutter inside the form",
          base(Prim::sphere(0.35f), hard(), cf3(-0.15f, 0, 0), cf3(0.15f, 0.1f, 0.05f)));
    check("cutter crossing the silhouette",
          base(Prim::sphere(0.6f), smooth(0.1f), cf3(0.6f, 0.2f, 0), cf3(0.95f, 0.2f, 0)));
    check("cutter entirely outside, and stays outside",
          base(Prim::sphere(0.4f), hard(), cf3(2.6f, 0, 0), cf3(2.9f, 0.3f, 0)));
    check("cutter moving outside -> inside",
          base(Prim::sphere(0.5f), smooth(0.09f), cf3(2.4f, 0, 0), cf3(0.1f, 0, 0)));
    check("cutter moving inside -> outside",
          base(Prim::sphere(0.5f), smooth(0.09f), cf3(0.1f, 0, 0), cf3(-2.4f, 0, 0)));
    check("cutter crossing the whole object in one command",
          base(Prim::box(cf3(0.3f, 1.6f, 1.6f)), hard(), cf3(-2.0f, 0, 0), cf3(2.0f, 0, 0)));
}

TEST_CASE("intersect delta: composition") {
    {
        // Under a group with a smooth blend, two deep. The group supports are
        // the dilation the bound has to carry, and a sibling inside the group
        // is material the edit must NOT be assumed to reach.
        Fixture f;
        Layer& l = f.doc.add_sdf_layer("body");
        f.layer = l.id;
        add_body(*l.sdf);
        Node outer;
        outer.is_group = true;
        outer.op = Op::Add;
        outer.blend = smooth(0.25f);
        const NodeId og = l.sdf->insert(outer);
        Node inner = outer;
        inner.blend = smooth(0.18f);
        const NodeId ig = l.sdf->insert(inner, og);
        l.sdf->insert(item(Prim::sphere(0.45f), cf3(1.3f, 0.4f, 0), Op::Add, smooth(0.1f)), ig);
        f.cutter =
            l.sdf->insert(item(Prim::sphere(0.6f), cf3(-0.4f, 0, 0), Op::Intersect, smooth(0.12f)),
                          ig);
        f.after = at(cf3(0.5f, 0.2f, 0));
        check("intersect under two blended groups", std::move(f));
    }
    {
        // Siblings on BOTH sides of the operand in the chain: what runs before
        // it is what its max() reads, what runs after it is what carries its
        // result on.
        Fixture f;
        Layer& l = f.doc.add_sdf_layer("body");
        f.layer = l.id;
        add_body(*l.sdf, smooth(0.1f));
        f.cutter = l.sdf->insert(
            item(Prim::sphere(0.75f), cf3(-0.3f, 0, 0), Op::Intersect, smooth(0.1f)));
        l.sdf->insert(item(Prim::sphere(0.4f), cf3(0.2f, -0.7f, 0.3f), Op::Add, smooth(0.14f)));
        l.sdf->insert(
            item(Prim::sphere(0.3f), cf3(-0.5f, 0.6f, 0.2f), Op::Subtract, smooth(0.08f)));
        f.after = at(cf3(0.45f, 0.15f, 0));
        check("siblings before and after, smooth", std::move(f));
    }
    {
        // A SECOND intersect the moved one has to live beside. Its own bound is
        // the layer extent and it does not move; nothing about it may leak into
        // the delta.
        Fixture f;
        Layer& l = f.doc.add_sdf_layer("body");
        f.layer = l.id;
        add_body(*l.sdf);
        l.sdf->insert(item(Prim::box(cf3(1.2f, 1.2f, 1.2f)), cf3(0, 0, 0), Op::Intersect,
                           smooth(0.1f)));
        f.cutter =
            l.sdf->insert(item(Prim::sphere(0.6f), cf3(-0.5f, 0, 0), Op::Intersect, hard()));
        f.after = at(cf3(0.5f, 0.1f, 0));
        check("two intersects, one of them moving", std::move(f));
    }
    {
        // Extended and material-creating modes downstream: relief and shell
        // read the running value the intersect produced.
        Fixture f;
        Layer& l = f.doc.add_sdf_layer("body");
        f.layer = l.id;
        add_body(*l.sdf);
        f.cutter =
            l.sdf->insert(item(Prim::sphere(0.7f), cf3(-0.4f, 0, 0), Op::Intersect, smooth(0.1f)));
        Node relief = item(Prim::sphere(0.35f), cf3(0.3f, 0.5f, 0.4f), Op::Relief, smooth(0.06f));
        relief.rounding = 0.05f;
        l.sdf->insert(relief);
        l.sdf->insert(item(Prim::sphere(0.55f), cf3(0.9f, 0, 0), Op::Paint, smooth(0.2f)));
        f.after = at(cf3(0.4f, 0.1f, 0.1f));
        check("relief and paint downstream", std::move(f));
    }
}

TEST_CASE("intersect delta: symmetry") {
    for (int radial = 0; radial < 2; ++radial) {
        Fixture f;
        Layer& l = f.doc.add_sdf_layer("body");
        f.layer = l.id;
        add_body(*l.sdf, smooth(0.08f));
        if (radial) {
            l.radial_count = 6;
            l.radial_axis = 1;
            l.radial_k = 0.12f;
        } else {
            l.mirror_axes = scene::kMirrorX | scene::kMirrorZ;
            l.mirror_k = 0.1f;
        }
        Node cut = item(Prim::sphere(0.5f), cf3(0.8f, 0.1f, 0.2f), Op::Intersect, smooth(0.1f));
        cut.mirror = true;
        f.cutter = l.sdf->insert(cut);
        f.after = at(cf3(0.35f, 0.4f, 0.5f));
        check(radial ? "radial 6 with a seam blend" : "mirror X+Z with a seam blend",
              std::move(f));
    }
}

TEST_CASE("intersect delta: layer composition") {
    // The layer holding the intersect is the base; a layer ABOVE it folds
    // smoothly, which is the support folds_from_layer_support contributes and
    // the one a bound that stopped at the layer would miss.
    struct Case {
        const char* name;
        Op fold;
    };
    const Case cases[] = {{"a smooth union above", Op::Add},
                          {"a smooth subtract above", Op::Subtract},
                          {"an intersecting layer above", Op::Intersect}};
    for (const Case& c : cases) {
        Fixture f;
        Layer& l = f.doc.add_sdf_layer("body");
        f.layer = l.id;
        add_body(*l.sdf);
        f.cutter =
            l.sdf->insert(item(Prim::sphere(0.7f), cf3(-0.4f, 0, 0), Op::Intersect, smooth(0.1f)));
        Layer& top = f.doc.add_sdf_layer("top");
        top.sdf->insert(item(Prim::box(cf3(1.5f, 1.5f, 1.5f)), cf3(0.3f, 0.2f, 0), Op::Add));
        top.composition = scene::LayerComposition{c.fold, smooth(0.22f), 0.03f};
        f.after = at(cf3(0.45f, 0.15f, 0));
        check(c.name, std::move(f));
    }
    {
        // ... and the same edit with the layer NOT at the bottom, so the fold
        // it enters through is applied to it as well.
        Fixture f;
        Layer& base_l = f.doc.add_sdf_layer("base");
        base_l.sdf->insert(item(Prim::box(cf3(1.6f, 0.3f, 1.6f)), cf3(0, -1.0f, 0), Op::Add));
        Layer& l = f.doc.add_sdf_layer("body");
        f.layer = l.id;
        add_body(*l.sdf);
        l.composition = scene::LayerComposition{Op::Add, smooth(0.3f), 0.0f};
        f.cutter =
            l.sdf->insert(item(Prim::sphere(0.7f), cf3(-0.4f, 0, 0), Op::Intersect, hard()));
        f.after = at(cf3(0.5f, 0.1f, 0));
        check("the layer folds smoothly onto one beneath it", std::move(f));
    }
}

TEST_CASE("intersect delta: instanced layers move every copy") {
    // Shared content: one edit lands once per instancing layer, each through
    // that layer's own transform. The delta is the union over them, exactly as
    // the influence bound is (issue #325).
    Fixture f;
    Layer& l = f.doc.add_sdf_layer("body");
    f.layer = l.id;
    add_body(*l.sdf);
    f.cutter =
        l.sdf->insert(item(Prim::sphere(0.7f), cf3(-0.4f, 0, 0), Op::Intersect, smooth(0.1f)));
    Layer* inst = f.doc.instance_layer(l.id, "body-copy");
    REQUIRE(inst != nullptr);
    inst->xform.position = cf3(3.2f, 0, 0);
    f.after = at(cf3(0.5f, 0.1f, 0));
    check("an instanced layer", std::move(f), 6.0f);
}

TEST_CASE("intersect delta: a very large blend radius") {
    // k = 0.6 on the operand and on the chain around it: four times the band,
    // and the term the bound is most sensitive to.
    Fixture f;
    Layer& l = f.doc.add_sdf_layer("body");
    f.layer = l.id;
    add_body(*l.sdf, smooth(0.6f));
    f.cutter = l.sdf->insert(item(Prim::sphere(0.9f), cf3(-0.5f, 0, 0), Op::Intersect,
                                  smooth(0.6f)));
    f.after = at(cf3(0.6f, 0.2f, 0));
    check("a very large blend radius", std::move(f), 5.0f);
}

TEST_CASE("intersect delta: a long smooth chain") {
    // THE SHAPE THE ISSUE IS ABOUT, at the scale that makes the chain pad mean
    // something: 200 smooth dabs in one serial chain, an intersect operand at
    // the root of it. `blend_cull_pad` measured the drag such a chain applies
    // to its own running value growing with its LENGTH -- 2.30k at 75 nodes,
    // 3.05k at 600 -- and a five-item fixture cannot show it.
    Fixture f;
    Layer& l = f.doc.add_sdf_layer("body");
    f.layer = l.id;
    clay_test::Lcg rng(0x4711u);
    for (int i = 0; i < 200; ++i) {
        const float t = static_cast<float>(i) / 199.0f;
        const cfloat3 p = cf3(std::cos(t * 9.0f) * (0.35f + 0.5f * t),
                              -0.7f + 1.4f * t + 0.1f * rng.range(-1.0f, 1.0f),
                              std::sin(t * 9.0f) * (0.35f + 0.5f * t));
        l.sdf->insert(item(Prim::sphere(0.22f), p, Op::Add, smooth(0.06f)));
    }
    f.cutter =
        l.sdf->insert(item(Prim::capped_cylinder(0.3f, 0.9f), cf3(-0.7f, 0.2f, 0),
                           Op::Intersect, smooth(0.08f)));
    f.after = at(cf3(0.55f, 0.2f, 0.1f));
    check("200 smooth dabs and one intersect", std::move(f), 3.0f);
}

TEST_CASE("intersect delta: the probe has teeth") {
    // THE TEST FOR THE TEST. Everything above passes; that is worth nothing
    // unless a bound one term short FAILS, and the cheapest term to remove is
    // the sweep itself -- the box of where the operand ENDED, with no memory of
    // where it was. It is exactly the mistake a one-sided bound would make, and
    // the probe must see it.
    Fixture f = base(Prim::sphere(0.55f), hard(), cf3(-0.8f, 0.1f, 0), cf3(0.8f, 0.1f, 0));
    const scene::Command cmd = move_cmd(f);
    const scene::Tape before_tape = scene::compile_document(f.doc);
    REQUIRE(scene::apply(f.doc, cmd).has_value());
    const std::optional<Aabb> after_only = scene::command_surface_delta_bound(f.doc, cmd);
    REQUIRE(after_only.has_value());
    const scene::Tape after_tape = scene::compile_document(f.doc);

    const Aabb outside_of = after_only->dilated(kBand);
    long flips = 0, band_changes = 0, outside = 0;
    for (const cfloat3& p : samples(*after_only, 4.0f, 0x471u)) {
        if (outside_of.contains(p)) continue;
        ++outside;
        const Class cb = classify(before_tape.eval(p).d), ca = classify(after_tape.eval(p).d);
        if (cb.sign != ca.sign) ++flips;
        if (cb.in_band != ca.in_band) ++band_changes;
    }
    REQUIRE(outside > 1000);
    CHECK(flips > 0);
    CHECK(band_changes > 0);
}

TEST_CASE("intersect delta: a thin wall near the band") {
    // A shell one band thick. Two nearby surfaces mean a point can be in the
    // band of one and not the other, which is where a classification flip is
    // cheapest to produce.
    Fixture f;
    Layer& l = f.doc.add_sdf_layer("body");
    f.layer = l.id;
    Node outer = item(Prim::sphere(1.0f), cf3(0, 0, 0), Op::Add);
    l.sdf->insert(outer);
    l.sdf->insert(item(Prim::sphere(0.86f), cf3(0, 0, 0), Op::Subtract));
    f.cutter =
        l.sdf->insert(item(Prim::box(cf3(0.5f, 0.5f, 0.5f)), cf3(-0.9f, 0, 0), Op::Intersect));
    f.after = at(cf3(0.9f, 0, 0));
    check("a thin wall", std::move(f));
}

// -- the refusals -----------------------------------------------------------

TEST_CASE("intersect delta: the cases that must fall back") {
    auto refuses = [](const char* name, Fixture f) {
        f.provable = false;
        check(name, std::move(f));
    };
    {
        Fixture f = base(Prim::sphere(0.4f), hard(), cf3(-0.4f, 0, 0), cf3(0.4f, 0, 0));
        f.doc.find_layer(f.layer)->sdf->find_mut(f.cutter)->repeat =
            scene::Repeat::grid_infinite(cf3(1.5f, 1.5f, 1.5f));
        refuses("an infinite grid repeat", std::move(f));
    }
    {
        Fixture f = base(Prim::plane(cf3(0, 1, 0), 0.2f), hard(), cf3(0, -0.3f, 0),
                         cf3(0, 0.3f, 0));
        refuses("an unbounded primitive", std::move(f));
    }
    {
        // A spatial morph ANYWHERE in the chain, not only on the operand: its
        // weight saturates, so it carries a beyond-band difference to any
        // distance.
        Fixture f = base(Prim::sphere(0.5f), hard(), cf3(-0.4f, 0, 0), cf3(0.4f, 0, 0));
        Node morph = item(Prim::sphere(0.6f), cf3(0.2f, 0.3f, 0), Op::TransitionRadial);
        morph.transition.r0 = 0.5f;
        morph.transition.r1 = 1.5f;
        f.doc.find_layer(f.layer)->sdf->insert(morph);
        refuses("a spatial morph downstream", std::move(f));
    }
    {
        Fixture f = base(Prim::sphere(0.5f), hard(), cf3(-0.4f, 0, 0), cf3(0.4f, 0, 0));
        f.doc.find_layer(f.layer)->sdf->find_mut(f.cutter)->deformers.push_back(
            scene::Deformer::twist(0.6f));
        refuses("a deformed operand", std::move(f));
    }
    {
        Fixture f = base(Prim::sphere(0.5f), hard(), cf3(-0.4f, 0, 0), cf3(0.4f, 0, 0));
        Node* cut = f.doc.find_layer(f.layer)->sdf->find_mut(f.cutter);
        cut->op = Op::Subtract;
        // A SUBTRACT is not a refusal of the proof -- it is a case with nothing
        // to prove, since its influence bound already IS this box. The delta
        // path stands aside so no other op's region changes at all.
        refuses("a subtracting operand needs no delta", std::move(f));
    }
}

TEST_CASE("intersect delta: a hidden or absent node keeps the conservative answer") {
    {
        Fixture f = base(Prim::sphere(0.5f), hard(), cf3(-0.4f, 0, 0), cf3(0.4f, 0, 0));
        f.doc.find_layer(f.layer)->sdf->find_mut(f.cutter)->visible = false;
        const std::optional<Aabb> b = scene::command_surface_delta_bound(f.doc, move_cmd(f));
        CHECK_FALSE(b.has_value());
    }
    {
        Fixture f = base(Prim::sphere(0.5f), hard(), cf3(-0.4f, 0, 0), cf3(0.4f, 0, 0));
        f.cutter = 9999;
        const std::optional<Aabb> b = scene::command_surface_delta_bound(f.doc, move_cmd(f));
        CHECK_FALSE(b.has_value());
    }
    {
        // Every other command kind, on the very node the fast path is about.
        Fixture f = base(Prim::sphere(0.5f), hard(), cf3(-0.4f, 0, 0), cf3(0.4f, 0, 0));
        CHECK_FALSE(scene::command_surface_delta_bound(
                        f.doc, scene::Command{scene::SetOpBlendCmd{f.layer, f.cutter, Op::Intersect,
                                                                   smooth(0.2f), 0.0f}})
                        .has_value());
        CHECK_FALSE(scene::command_surface_delta_bound(
                        f.doc, scene::Command{scene::SetPrimCmd{f.layer, f.cutter,
                                                                Prim::sphere(0.9f)}})
                        .has_value());
        CHECK_FALSE(scene::command_surface_delta_bound(
                        f.doc, scene::Command{scene::RemoveNodeCmd{f.layer, f.cutter}})
                        .has_value());
    }
}

TEST_CASE("intersect delta: the bound is smaller than the layer, and grows with the sweep") {
    // The performance claim, as a claim about the BOX rather than about a
    // clock: the delta of a small move is a small fraction of what the
    // influence bound dirties, and it grows with the move rather than with the
    // layer.
    Fixture near_move = base(Prim::sphere(0.4f), hard(), cf3(-0.2f, 0, 0), cf3(0.2f, 0, 0));
    const scene::Command cmd = move_cmd(near_move);
    scene::LayerExtent memo;
    const Aabb conservative = scene::command_influence_bound(near_move.doc, cmd, &memo);
    const std::optional<Aabb> before = scene::command_surface_delta_bound(near_move.doc, cmd);
    REQUIRE(scene::apply(near_move.doc, cmd).has_value());
    const std::optional<Aabb> after = scene::command_surface_delta_bound(near_move.doc, cmd);
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());
    Aabb delta = *before;
    delta.expand(*after);

    auto volume = [](const Aabb& b) {
        const cfloat3 e = b.extent();
        return static_cast<double>(e.x) * e.y * e.z;
    };
    CHECK(volume(delta) < 0.35 * volume(conservative));
}
