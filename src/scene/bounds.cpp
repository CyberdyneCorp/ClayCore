#include "clay/scene/bounds.h"

namespace clay {
namespace scene {

using kernel::cf3;
using kernel::cfloat3;
using math::Aabb;

Aabb prim_local_bounds(const Node& item) {
    const float* q = item.prim.params;
    Aabb b;
    auto sym = [&](float x, float y, float z) {
        b.expand(cf3(-x, -y, -z));
        b.expand(cf3(x, y, z));
    };
    switch (item.prim.type) {
        case PrimType::Sphere: sym(q[0], q[0], q[0]); break;
        case PrimType::Box: sym(q[0], q[1], q[2]); break;
        case PrimType::RoundBox: sym(q[0], q[1], q[2]); break;
        case PrimType::BoxFrame: sym(q[0], q[1], q[2]); break;
        case PrimType::Torus: sym(q[0] + q[1], q[1], q[0] + q[1]); break;
        case PrimType::Capsule: {
            cfloat3 a = cf3(q[0], q[1], q[2]), c = cf3(q[3], q[4], q[5]);
            float r = q[6];
            b.expand(kernel::cmin(a, c) - cf3(r, r, r));
            b.expand(kernel::cmax(a, c) + cf3(r, r, r));
            break;
        }
        case PrimType::CappedCylinder: sym(q[0], q[1], q[0]); break;
        case PrimType::RoundedCylinder: sym(q[0], q[2], q[0]); break;
        case PrimType::CappedCone: sym(kernel::cmax(q[1], q[2]), q[0], kernel::cmax(q[1], q[2])); break;
        case PrimType::RoundCone: {
            float r = kernel::cmax(q[0], q[1]);
            b.expand(cf3(-r, -q[0], -r));
            b.expand(cf3(r, q[2] + q[1], r));
            break;
        }
        case PrimType::Ellipsoid: sym(q[0], q[1], q[2]); break;
        case PrimType::Octahedron: sym(q[0], q[0], q[0]); break;
        case PrimType::HexPrism: sym(q[0] * 1.1547005f, q[0] * 1.1547005f, q[1]); break;
        case PrimType::Pyramid: {
            b.expand(cf3(-0.5f, 0.0f, -0.5f));
            b.expand(cf3(0.5f, q[0], 0.5f));
            break;
        }
        case PrimType::Stroke: {
            for (const StrokePoint& p : item.stroke) {
                b.expand(p.pos - cf3(p.radius, p.radius, p.radius));
                b.expand(p.pos + cf3(p.radius, p.radius, p.radius));
            }
            // within-stroke smoothing widens the surface by its support
            if (item.stroke_blend_k > 0.0f)
                b = b.dilated(kernel::csmin_quadratic_support(item.stroke_blend_k));
            break;
        }
    }
    return b;
}

Aabb item_influence_bound(const Node& item, const Layer& layer) {
    if (item.op == Op::Intersect) return Aabb::infinite();
    Aabb local = prim_local_bounds(item);
    if (local.empty()) return local;

    math::Transform world = layer.xform * item.xform;
    Aabb bound = local.transformed(world.matrix());
    if (item.mirror && layer.mirror_axes != 0) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!(layer.mirror_axes & (1u << axis))) continue;
            math::cfloat4x4 m = math::mul(
                layer.xform.matrix(),
                math::mul(math::reflection_matrix(axis), item.xform.matrix()));
            bound.expand(local.transformed(m));
        }
        bound = bound.dilated(kernel::csmin_quadratic_support(layer.mirror_k));
    }
    // Rounding is authored in item-local units (tape emits round*scale);
    // erosion (negative rounding) shrinks the surface, never the bound.
    // Paint fades over max(profile support, k). Extended modes deviate
    // within their documented support of the item surface (kernel/tape.h) —
    // for groove/tongue that is the rounding again (rb), on top of the
    // rounding dilation the item field already carries.
    float round_world = item.rounding * world.scale;
    float combine = op_is_extended(item.op)
                        ? kernel::ccombine_extended_support(static_cast<int>(item.op),
                                                            item.blend.k, round_world)
                        : kernel::cmax(item.blend.support(), item.blend.k);
    return bound.dilated(kernel::cmax(round_world, 0.0f) + combine);
}

Aabb node_influence_bound(const SdfContent& content, NodeId id, const Layer& layer) {
    const Node* n = content.find(id);
    if (!n || !n->visible) return Aabb{};
    if (!n->is_group) return item_influence_bound(*n, layer);
    if (n->op == Op::Intersect) return Aabb::infinite();
    Aabb b;
    for (NodeId c : n->children) {
        Aabb cb = node_influence_bound(content, c, layer);
        if (cb.is_infinite()) return Aabb::infinite();
        b.expand(cb);
    }
    // Extended-op groups: the subtree field is not rounded, so rb comes
    // straight from the group's rounding scaled into world units.
    float support = op_is_extended(n->op)
                        ? kernel::ccombine_extended_support(
                              static_cast<int>(n->op), n->blend.k,
                              n->rounding * layer.xform.scale)
                        : n->blend.support();
    return b.empty() ? b : b.dilated(support);
}

Aabb layer_influence_bound(const Layer& layer) {
    Aabb b;
    if (!layer.sdf) return b;
    for (NodeId id : layer.sdf->roots) {
        Aabb nb = node_influence_bound(*layer.sdf, id, layer);
        if (nb.is_infinite()) return Aabb::infinite();
        b.expand(nb);
    }
    return b;
}

}  // namespace scene
}  // namespace clay
