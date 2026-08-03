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

inline CTapeValue ref_eval_item(const scene::Node& item, const scene::Layer& layer, cfloat3 p) {
    using namespace kernel;
    math::Transform world = layer.xform * item.xform;

    auto eval_at = [&](cfloat3 lp) {
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
        } else {
            d = ctape_prim_dist(static_cast<unsigned int>(item.prim.type), item.prim.params,
                                nullptr, lp);
        }
        return d * world.scale - item.rounding * world.scale;
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

inline bool ref_eval_group(const scene::Node& g, const scene::SdfContent& content,
                           const scene::Layer& layer, cfloat3 p, CTapeValue& acc,
                           bool have_acc) {
    using namespace kernel;
    if (g.op == scene::Op::None)
        return ref_eval_list(g.children, content, layer, p, acc, have_acc);
    if (!have_acc && g.op != scene::Op::Add) return have_acc;
    CTapeValue sub;
    bool has_sub = ref_eval_list(g.children, content, layer, p, sub, false);
    if (!has_sub) return have_acc;
    if (have_acc)
        acc = ctape_combine_values(acc, sub, static_cast<int>(g.op), static_cast<int>(g.blend.profile),
                            g.blend.k);
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
        if (!have_acc && n->op != scene::Op::Add) continue;
        CTapeValue item = ref_eval_item(*n, layer, p);
        if (have_acc)
            acc = ctape_combine_values(acc, item, static_cast<int>(n->op),
                                static_cast<int>(n->blend.profile), n->blend.k);
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
            acc = ctape_combine_values(acc, lv, ccombine_add, cblend_hard, 0.0f);
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

}  // namespace clay_test
