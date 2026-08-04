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
            float rec[6] = {static_cast<float>(def.type), def.k, def.a,
                            def.b, def.c, static_cast<float>(def.ease)};
            offset += ctape_deform_offset(rec, lp);
            lp = ctape_deform_point(rec, lp);
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

    CTapeValue v;
    v.color = item.color;
    v.d = eval_at(world.apply_inverse(p));
    if (item.mirror && layer.mirror_axes != 0) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!(layer.mirror_axes & (1u << axis))) continue;
            cfloat3 lq = layer.xform.apply_inverse(p);
            if (axis == 0) lq.x = -lq.x;
            if (axis == 1) lq.y = -lq.y;
            if (axis == 2) lq.z = -lq.z;
            float dm = eval_at(item.xform.apply_inverse(lq));
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

inline CTapeValue ref_eval_document(const scene::Document& doc, cfloat3 p) {
    using namespace kernel;
    CTapeValue acc;
    bool have_acc = false;
    for (const scene::Layer& layer : doc.layers) {
        if (!layer.visible || layer.kind != scene::LayerKind::Sdf || !layer.sdf) continue;
        CTapeValue lv;
        if (!ref_eval_list(layer.sdf->roots, *layer.sdf, layer, p, lv, false)) continue;
        if (have_acc)
            acc = ctape_combine_values(acc, lv, ccombine_add, cblend_hard, 0.0f, 0.0f);
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


}  // namespace clay_test
