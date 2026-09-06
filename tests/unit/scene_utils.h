#pragma once

// Scene test helpers: a reference tree evaluator (recursive, Transform-based
// — independent of the tape compiler's matrix emission) and document
// builders. The reference reuses the kernel's prim/combine dispatch so tape
// tests isolate exactly what the COMPILER contributes: traversal order,
// transform inversion, mirror emission, culling.

#include <vector>

#include "clay/kernel/tape.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

namespace clay_test {

using namespace clay;
using kernel::cfloat3;
using kernel::CTapeValue;

// Mirrors the compiler's empty-accumulator seeding: material-creating modes
// (shell, replace) combine against the far field (ctape_empty, carrying the
// node's color) instead of being skipped.
inline CTapeValue ref_combine(const CTapeValue* acc, kernel::cfloat3 seed_color, CTapeValue item,
                              scene::Op op, scene::Blend blend, float rb) {
    CTapeValue a;
    a.d = CLAY_TAPE_FAR;
    a.color = seed_color;
    if (acc) a = *acc;
    return kernel::ctape_combine_values(a, item, static_cast<int>(op),
                                        static_cast<int>(blend.profile), blend.k, rb);
}

inline CTapeValue ref_eval_item(const scene::Node& item, const scene::Layer& layer, cfloat3 p) {
    using namespace kernel;
    math::Transform world = layer.xform * item.xform;

    auto eval_at = [&](cfloat3 lp) {
        // mirror the tape's deformer chain: warp in authoring order, then add
        // each deformer's distance contribution
        float offset = 0.0f;
        for (const scene::Deformer& def : item.deformers) {
            // Full tape width. A six-float record left the extension slots off
            // the end, so every deformer that uses them — noise, grab, pose,
            // pose_line, magnify, bend_linear — was compared against whatever
            // sat past the array (GCC flagged the rec[6] read; the project
            // demotes -Warray-bounds for a libstdc++ false positive, which hid
            // this real one).
            float rec[CLAY_TAPE_DEFORM_FLOATS] = {static_cast<float>(def.type), def.k, def.a,
                                                  def.b, def.c, static_cast<float>(def.ease)};
            for (int e = 0; e < scene::Deformer::ext_count(def.type); ++e) rec[6 + e] = def.ext[e];
            // bend_curve reads a guide the COMPILER wrote into the blob, with
            // parallel-transported frames. Mirroring that here would be a
            // second implementation of the very thing this reference exists to
            // check independently, so it is left out: an item using one warps
            // here as if undeformed and fails the comparison loudly rather
            // than agreeing by construction. Its evidence is direct instead —
            // a straight guide is the identity, a circular one is `bend`.
            // ...and a lattice, for the same reason: its cage is blob-carried.
            // ...and an alpha, whose stamp is blob-carried too.
            if (def.type == kernel::cdeform_bend_curve || def.type == kernel::cdeform_lattice ||
                def.type == kernel::cdeform_alpha)
                continue;
            offset += ctape_deform_offset(rec, nullptr, lp);
            lp = ctape_deform_point(rec, nullptr, lp);
        }
        float d;
        if (item.prim.type == scene::PrimType::Stroke) {
            std::vector<float> pts;
            for (const scene::StrokePoint& sp : item.stroke) {
                pts.push_back(sp.pos.x);
                pts.push_back(sp.pos.y);
                pts.push_back(sp.pos.z);
                pts.push_back(sp.radius);
            }
            d = ctape_stroke_dist(pts.data(), static_cast<int>(item.stroke.size()), lp,
                                  item.stroke_blend_k);
        } else if (scene::prim_is_lift(item.prim.type)) {
            std::vector<float> prof(CLAY_TAPE_PROFILE_FLOATS + 1, 0.0f);
            std::vector<float> verts;
            prof[0] = static_cast<float>(item.profile.type);
            for (int i = 0; i < 4; ++i) prof[i + 1] = item.profile.params[i];
            if (item.profile.is_polygon()) {
                prof[1] = 0.0f;
                prof[2] = static_cast<float>(item.profile_points.size());
                for (const cfloat2& v : item.profile_points) {
                    verts.push_back(v.x);
                    verts.push_back(v.y);
                }
            }
            prof[CLAY_TAPE_PROFILE_FLOATS] = item.prim.params[0];
            d = ctape_prim_dist(static_cast<unsigned int>(item.prim.type), prof.data(),
                                verts.data(), lp);
        } else {
            d = ctape_prim_dist(static_cast<unsigned int>(item.prim.type), item.prim.params,
                                nullptr, lp);
        }
        return (d + offset) * world.scale - item.rounding * world.scale;
    };

    // repetition maps the local point before any deformer; radial arrays
    // need the nearest sector and its neighbour
    auto eval_repeated = [&](cfloat3 lp) {
        const scene::Repeat& r = item.repeat;
        if (!r.active()) return eval_at(lp);
        float rec[7] = {static_cast<float>(r.type), r.spacing.x, r.spacing.y, r.spacing.z,
                        r.counts.x, r.counts.y, r.counts.z};
        if (r.type == kernel::crepeat_radial) {
            float d0 = eval_at(ctape_repeat_point(rec, lp, 0));
            int neighbour = crep_radial_neighbor(lp, static_cast<int>(r.spacing.x));
            return cmin(d0, eval_at(ctape_repeat_point(rec, lp, neighbour)));
        }
        return eval_at(ctape_repeat_point(rec, lp, 0));
    };

    CTapeValue v;
    v.color = item.color;
    v.d = eval_repeated(world.apply_inverse(p));
    if (item.mirror && layer.mirror_axes != 0) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!(layer.mirror_axes & (1u << axis))) continue;
            cfloat3 lq = layer.xform.apply_inverse(p);
            if (axis == 0) lq.x = -lq.x;
            if (axis == 1) lq.y = -lq.y;
            if (axis == 2) lq.z = -lq.z;
            float dm = eval_repeated(item.xform.apply_inverse(lq));
            v.d = layer.mirror_k > 0.0f ? csmin_quadratic(v.d, dm, layer.mirror_k)
                                        : cmin(v.d, dm);
        }
    }
    return v;
}

inline bool ref_eval_list(const std::vector<scene::NodeId>& ids,
                          const scene::SdfContent& content, const scene::Layer& layer, cfloat3 p,
                          CTapeValue& acc, bool have_acc);

// Mirror of the tape's transition combine (a lerp of both operands).
inline CTapeValue ref_transition(CTapeValue a, CTapeValue b, const scene::Node& n, cfloat3 p) {
    float w = n.op == scene::Op::TransitionLinear
                  ? kernel::ctransition_linear_weight(p, n.transition.a, n.transition.b,
                                                      n.transition.ease)
                  : kernel::ctransition_radial_weight(p, n.transition.r0, n.transition.r1,
                                                      n.transition.ease);
    CTapeValue r;
    r.d = kernel::cmix(a.d, b.d, w);
    r.color = kernel::cmix(a.color, b.color, w);
    return r;
}

inline bool ref_eval_group(const scene::Node& g, const scene::SdfContent& content,
                           const scene::Layer& layer, cfloat3 p, CTapeValue& acc,
                           bool have_acc) {
    using namespace kernel;
    if (g.op == scene::Op::None)
        return ref_eval_list(g.children, content, layer, p, acc, have_acc);
    if (!have_acc && g.op != scene::Op::Add && !scene::op_creates_material(g.op))
        return have_acc;
    CTapeValue sub;
    bool has_sub = ref_eval_list(g.children, content, layer, p, sub, false);
    if (!has_sub) return have_acc;
    float rb = g.rounding * layer.xform.scale;
    if (have_acc)
        acc = ref_combine(&acc, g.color, sub, g.op, g.blend, rb);
    else if (g.op != scene::Op::Add)
        acc = ref_combine(nullptr, g.color, sub, g.op, g.blend, rb);
    else
        acc = sub;
    return true;
}

inline bool ref_eval_list(const std::vector<scene::NodeId>& ids,
                          const scene::SdfContent& content, const scene::Layer& layer, cfloat3 p,
                          CTapeValue& acc, bool have_acc) {
    using namespace kernel;
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n || !n->visible) continue;
        if (n->is_group) {
            have_acc = ref_eval_group(*n, content, layer, p, acc, have_acc);
            continue;
        }
        if (!have_acc && n->op != scene::Op::Add && !scene::op_creates_material(n->op))
            continue;
        CTapeValue item = ref_eval_item(*n, layer, p);
        float rb = n->rounding * layer.xform.scale * n->xform.scale;
        if (have_acc && scene::op_is_transition(n->op))
            acc = ref_transition(acc, item, *n, p);
        else if (have_acc)
            acc = ref_combine(&acc, n->color, item, n->op, n->blend, rb);
        else if (n->op != scene::Op::Add)
            acc = ref_combine(nullptr, n->color, item, n->op, n->blend, rb);
        else
            acc = item;
        have_acc = true;
    }
    return have_acc;
}

// WHAT THIS IS, SAID EXACTLY, because the word it used to carry was
// "independent" and that was an overclaim (fold-the-layers-with-an-operator,
// design.md §2 row 10).
//
// It is a DIFFERENTIAL, not an independent oracle. It shares with the compiler
// everything the file header lists -- the kernel's prim and combine dispatch --
// and, since layers gained a composition, the FOLD RULE as well: which layer is
// first, what a layer that is not first does with an absent accumulator, and
// that an absent layer value is the far field. Those were written from the
// spec's statements and are spelled differently here (this folds the far field
// unconditionally where `emit_layer_fold` asks `fold_changes_an_empty_layer`
// and skips the identity cases), but they were derived by reading
// `compile_and_fold_layer`, and a reader is owed that rather than a claim of
// independence.
//
// WHAT IT THEREFORE CATCHES: everything the COMPILER contributes and this does
// not have -- traversal order, transform inversion, mirror emission, culling,
// checkpoints, the tape's stack discipline -- which is a large part of this
// change and is why the fold's own tests still run through here. A change to
// one side and not the other fails loudly.
//
// WHAT IT CANNOT CATCH, stated so nobody counts it twice: a fold rule that is
// wrong in the same way on both sides, and a wrong `ctape_combine_values`. The
// rule's own evidence has to come from somewhere neither reaches -- the
// analytic expectations in test_layer_fold.cpp (an intersecting layer over
// nothing is nothing; a subtract removes exactly the cutter), the item/layer
// parity fixtures in test_layer_parity.cpp, which compare a document folded by
// LAYERS against the same shape folded by ITEMS in one layer, and the C ABI
// gates in test_layer_gates.cpp.
inline CTapeValue ref_eval_document(const scene::Document& doc, cfloat3 p) {
    using namespace kernel;
    CTapeValue acc;
    bool have_acc = false;
    // FIRST IS A PROPERTY OF THE LAYER LIST, not of what the layers beneath
    // happened to produce. Reading it off `have_acc` is exactly the defect this
    // evaluator exists to catch in the compiler, so it must not repeat it: a
    // document whose lower layers are empty would then show an intersecting
    // layer whole, and agree with a compiler that did the same.
    bool first = true;
    for (const scene::Layer& layer : doc.layers) {
        if (!layer.visible || layer.kind != scene::LayerKind::Sdf || !layer.sdf) continue;
        const bool is_first = first;
        first = false;
        const scene::LayerComposition& lc = layer.composition;
        // An absent accumulator under a layer that is not the first: the item
        // rule, which is what compile_and_fold_layer lifts (a carving operator
        // over nothing is nothing; Shell and Replace fold against the far
        // field; a union is the layer itself).
        if (!is_first && !have_acc && lc.op != scene::Op::Add &&
            !scene::op_creates_material(lc.op))
            continue;
        CTapeValue lv;
        if (!ref_eval_list(layer.sdf->roots, *layer.sdf, layer, p, lv, false)) {
            // A layer whose chain produced nothing IS the far field, and this
            // says so directly rather than deciding which operators may be
            // skipped: combining with FAR is already a no-op for the ones the
            // compiler skips, and it is not for the ones it does not.
            //
            // With nothing on either side there is nothing to fold at all --
            // unless the operator makes material out of the far field (Shell,
            // Replace), which the seed below hands it.
            if (!have_acc && !(!is_first && scene::op_creates_material(lc.op))) continue;
            lv.d = CLAY_TAPE_FAR;
            lv.color = kernel::cf3(1.0f, 1.0f, 1.0f);
        }
        // THE LAYER'S OWN COMPOSITION, and the first visible SDF layer's is not
        // applied -- it initialises. Reading the field here rather than folding
        // a hard Add is what stops this evaluator agreeing with the compiler
        // for the wrong reason: one that unions whatever the document says
        // agrees only while every fixture unions, which is the one condition
        // under which a fold bug is invisible. It does NOT make the two
        // independent -- see the note above the function for what this shares
        // with the compiler and what therefore has to be proved elsewhere.
        const float rb = lc.rounding * scene::layer_distance_scale(layer);
        if (is_first)
            acc = lv;
        else if (have_acc)
            acc = ctape_combine_values(acc, lv, static_cast<int>(lc.op),
                                       static_cast<int>(lc.blend.profile), lc.blend.k, rb);
        else if (lc.op != scene::Op::Add)
            // Shell or Replace over an absent accumulator: the far field is the
            // left operand, exactly as ref_eval_list seeds one for an item.
            acc = ref_combine(nullptr, kernel::cf3(1.0f, 1.0f, 1.0f), lv, lc.op, lc.blend, rb);
        else
            acc = lv;
        have_acc = true;
    }
    if (!have_acc) {
        acc.d = CLAY_TAPE_FAR;
        acc.color = kernel::cf3(0.5f, 0.5f, 0.5f);
    }
    return acc;
}


// -- shared scene builders --------------------------------------------------

using scene::Blend;
using scene::BlendProfile;
using scene::Document;
using scene::kMirrorX;
using scene::Layer;
using scene::Node;
using scene::NodeId;
using scene::Op;
using scene::Prim;
using scene::SdfContent;
using kernel::cf3;

inline Node item(Prim prim, cfloat3 pos, Op op = Op::Add, Blend blend = {}) {
    Node n;
    n.prim = prim;
    n.xform.position = pos;
    n.op = op;
    n.blend = blend;
    return n;
}

// A document exercising the whole vocabulary: nested groups (4 deep),
// mirror, blob, every blend profile, all ops, layer + item transforms,
// and an instanced layer.
inline Document gnarly_document() {
    Document doc;
    Layer& body = doc.add_sdf_layer("body");
    body.xform.position = cf3(0.2f, -0.1f, 0.05f);
    body.xform.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), 0.4f);
    body.mirror_axes = kMirrorX;
    body.mirror_k = 0.08f;
    SdfContent& c = *body.sdf;

    c.insert(item(Prim::sphere(1.0f), cf3(0, 0, 0)));
    c.insert(item(Prim::box(cf3(0.5f, 0.4f, 0.6f)), cf3(0.6f, 0.3f, 0),
                  Op::Add, Blend{BlendProfile::Quadratic, 0.1f}));
    c.insert(item(Prim::capped_cylinder(0.3f, 0.8f), cf3(-0.4f, 0.5f, 0), Op::Subtract,
                  Blend{BlendProfile::Cubic, 0.05f}));
    c.insert(item(Prim::torus(0.7f, 0.15f), cf3(0, 0.9f, 0), Op::Add,
                  Blend{BlendProfile::Chamfer, 0.07f}));
    c.insert(item(Prim::ellipsoid(cf3(0.4f, 0.2f, 0.3f)), cf3(0, -0.8f, 0.2f), Op::Add,
                  Blend{BlendProfile::Circular, 0.06f}));

    // mirrored item
    Node ear = item(Prim::round_cone(0.25f, 0.1f, 0.4f), cf3(0.9f, 0.6f, 0));
    ear.mirror = true;
    ear.blend = Blend{BlendProfile::Quadratic, 0.05f};
    c.insert(ear);

    // stroke
    Node stroke;
    stroke.prim = Prim::stroke();
    stroke.stroke = {{cf3(-1, 0, 0.5f), 0.2f},
                     {cf3(-0.5f, 0.4f, 0.5f), 0.15f},
                     {cf3(0, 0.2f, 0.6f), 0.18f}};
    stroke.stroke_blend_k = 0.03f;
    stroke.blend = Blend{BlendProfile::Quadratic, 0.08f};
    c.insert(stroke);

    // nested groups 4 deep: g1 > g2 > g3 > g4
    Node g1;
    g1.is_group = true;
    g1.op = Op::Add;
    g1.blend = Blend{BlendProfile::Quadratic, 0.1f};
    NodeId g1id = c.insert(g1);
    Node g2;
    g2.is_group = true;
    g2.op = Op::None;  // inline
    NodeId g2id = c.insert(g2, g1id);
    Node g3;
    g3.is_group = true;
    g3.op = Op::Subtract;
    g3.blend = Blend{BlendProfile::Quadratic, 0.04f};
    NodeId g3id = c.insert(g3, g2id);
    Node g4;
    g4.is_group = true;
    g4.op = Op::Add;
    NodeId g4id = c.insert(g4, g3id);
    c.insert(item(Prim::octahedron(0.5f), cf3(0, 0, -0.8f)), g2id);
    c.insert(item(Prim::hex_prism(0.3f, 0.2f), cf3(0.2f, 0, -0.8f)), g3id);
    c.insert(item(Prim::sphere(0.2f), cf3(0.1f, 0.1f, -0.7f)), g4id);

    // paint pass
    c.insert(item(Prim::sphere(0.5f), cf3(0.5f, 0.5f, 0.5f), Op::Paint,
                  Blend{BlendProfile::Quadratic, 0.1f}));

    // second layer + instance of the first
    Layer& base = doc.add_sdf_layer("base");
    base.xform.position = cf3(0, -1.6f, 0);
    base.sdf->insert(item(Prim::box(cf3(1.5f, 0.2f, 1.5f)), cf3(0, 0, 0)));

    Layer* inst = doc.instance_layer(doc.layers[0].id, "body-instance");

    inst->xform.position = cf3(3, 0, 0);

    return doc;
}

// The same document with its layers FOLDED rather than unioned
// (fold-the-layers-with-an-operator, design.md §2 row 10). `gnarly_document`
// exercises the whole ITEM vocabulary against one inter-layer combine -- the
// hard union every layer had before compositions existed -- so on its own it is
// the fixture under which a fold defect is invisible.
//
// Every kind of fold the setter accepts appears once, and each is chosen to be
// VISIBLE over the sampled domain rather than merely set:
//   * the first visible layer carries a Subtract that MUST NOT BE APPLIED (it
//     initialises), which is the rule a per-brick cull can otherwise flip;
//   * `base`, the plinth under the body, SUBTRACTS with a smooth radius and a
//     rounding -- so both terms of the fold are non-zero and the rounding is
//     the one that scales with the layer;
//   * `body-instance` unions SMOOTHLY, moved in to x = 1.9 so that there IS a
//     seam for the radius to bulge at -- at its own x = 3 the two clusters are
//     further apart than any radius the fold carries, and the layer would be
//     composed in name only;
//   * `clip`, added on top, INTERSECTS a box that contains the body and not
//     the instance -- the operator whose far field wins, and the one that
//     turns a skipped fold into material that should not be there.
inline Document composed_gnarly_document() {
    Document doc = gnarly_document();
    doc.layers[0].composition =
        scene::LayerComposition{Op::Subtract, Blend{BlendProfile::Quadratic, 0.2f}, 0.05f};
    doc.layers[1].composition =
        scene::LayerComposition{Op::Subtract, Blend{BlendProfile::Quadratic, 0.15f}, 0.04f};
    doc.layers[2].composition =
        scene::LayerComposition{Op::Add, Blend{BlendProfile::Chamfer, 0.5f}, 0.0f};

    // The instance moves in from x = 3 to x = 1.9: at 3 the two clusters are
    // further apart than any radius the fold could carry, so its composition
    // would be the identity everywhere and the layer would be composed in name
    // only. Here the two surfaces are about 0.3 apart and the smooth union has
    // a seam to bulge at.
    doc.layers[2].xform.position = cf3(1.9f, 0, 0);

    Layer& clip = doc.add_sdf_layer("clip");
    clip.sdf->insert(item(Prim::box(cf3(2.2f, 2.2f, 2.2f)), cf3(0, 0, 0)));
    clip.composition =
        scene::LayerComposition{Op::Intersect, Blend{BlendProfile::Quadratic, 0.1f}, 0.0f};
    return doc;
}


}  // namespace clay_test
