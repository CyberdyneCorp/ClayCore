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

// World-space influence bound of one item within a layer (includes mirror
// copies, rounding, and the item's blend support).
math::Aabb item_influence_bound(const Node& item, const Layer& layer);

// Influence bound of any node (recursive union for groups, dilated by the
// group's blend support; infinite for intersect anywhere in the subtree).
math::Aabb node_influence_bound(const SdfContent& content, NodeId id, const Layer& layer);

// Whole-layer bound (union of root node bounds).
math::Aabb layer_influence_bound(const Layer& layer);

}  // namespace scene
}  // namespace clay
