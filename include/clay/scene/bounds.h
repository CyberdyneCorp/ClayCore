#pragma once

// Influence bounds (scene-model spec): conservative world-space AABB outside
// which an item/group cannot change field values — shape AABB transformed to
// world, dilated by rounding + blend support. Everything downstream (brick
// dirtying, per-brick tape culling, locality guarantee) builds on this.
//
// Intersect items are the exception: max(d_prev, d_item) can change the
// field arbitrarily far away, so their influence is infinite.

#include "clay/math/geom.h"
#include "clay/scene/document.h"

namespace clay {
namespace scene {

// Local-space AABB of a primitive (before transform, rounding, blend).
math::Aabb prim_local_bounds(const Node& item);

// Lipschitz factor of an item's deformer chain (1 when undeformed). A warped
// field UNDERESTIMATES distance by at most this factor, so the item's
// influence reaches L times further than its blend support alone suggests —
// both the influence bound and the tape's tracked field info use this.
float deformer_lipschitz(const Node& item);

// World-space GEOMETRY bound of one item: shape AABB (deformed, mirrored,
// transformed) dilated by rounding and blend support. Always finite — this
// is what meshing and raycast clipping want.
math::Aabb item_geometry_bound(const Node& item, const Layer& layer);

// World-space INFLUENCE bound: the geometry bound for local ops, infinite
// for ops that change the field arbitrarily far away (intersect, the spatial
// morphs). This is what per-brick culling must consult.
math::Aabb item_influence_bound(const Node& item, const Layer& layer);

// Influence bound of any node (recursive union for groups, dilated by the
// group's blend support; infinite for intersect anywhere in the subtree).
math::Aabb node_influence_bound(const SdfContent& content, NodeId id, const Layer& layer);

// Whole-layer bound (union of root node bounds).
math::Aabb layer_influence_bound(const Layer& layer);

}  // namespace scene
}  // namespace clay
