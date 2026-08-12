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

// Local-space AABB of an item's SHAPE: the primitive bound after its deformer
// chain and repetition, still before transform, rounding and blend. Callers
// that want the shape's extent (picking, zoom-to-selection) need this rather
// than prim_local_bounds, which describes one undeformed, unrepeated copy.
math::Aabb item_local_bounds(const Node& item);

// Lipschitz factor of an item's deformer chain (1 when undeformed). A warped
// field UNDERESTIMATES distance by at most this factor, so the item's
// influence reaches L times further than its blend support alone suggests —
// both the influence bound and the tape's tracked field info use this.
float deformer_lipschitz(const Node& item);

// Steepest slope of an easing curve, measured by dense sampling. The curves are
// arbitrary — back and elastic overshoot — so a constant would not be a safe
// bound. Shared by the transition weight and the region deformers.
float ease_max_slope(std::uint8_t ease);

// Half-extent of a 2D profile about its own origin. Shared because bounds and
// the compiler's Lipschitz estimate both need it once per profile, and a
// second copy of the switch would be a second place for a new profile type to
// be forgotten.
kernel::cfloat2 profile_extent_of(const Profile& profile,
                                  const std::vector<kernel::cfloat2>& points);

// Whether the item's deformer chain costs exactness for a reason the Lipschitz
// factor does not capture (elongation on an asymmetric primitive).
bool deformers_break_exactness(const Node& item);

// World-space GEOMETRY bound of one item: shape AABB (deformed, mirrored,
// transformed) dilated by rounding and blend support. Always finite — this
// is what meshing and raycast clipping want.
math::Aabb item_geometry_bound(const Node& item, const Layer& layer);

// Whether an item's influence is confined to its own geometry. False means the
// item changes the field arbitrarily far away — a non-local op, an infinite
// grid repeat, or a primitive with no finite extent — and no finite bound may
// be claimed for it, so culling must never drop it.
//
// Exposed so a caller that already holds the geometry bound can decide
// cullability without recomputing the bound. It is the ONE definition of the
// test: item_influence_bound below is written in terms of it, so a new
// non-local op or unbounded primitive cannot leave a second copy stale (which
// would silently drop the item from per-brick tapes only).
bool item_influence_is_local(const Node& item);

// World-space INFLUENCE bound: the geometry bound for local ops, infinite
// for ops that change the field arbitrarily far away (intersect, the spatial
// morphs). This is what per-brick culling must consult.
math::Aabb item_influence_bound(const Node& item, const Layer& layer);

// Whether this item is a volume placed with Replace that asked for a
// feathered placement. The ONE definition of the test the compiler's mirror
// skip, combine choice and field-info fold share with the cull index's
// chain-pruning refusal, so they cannot disagree.
bool item_is_feathered_replace(const Node& item);

// The extra width the cull test needs when this content holds a feathered
// volume replace (see tape_build.cpp for why the feather reaches past the
// caller's dilation). Zero — the common case — leaves the cull test as the
// caller built it.
float feather_cull_pad(const SdfContent& content, const Layer& layer);

// Influence bound of any node (recursive union for groups, dilated by the
// group's blend support; infinite for intersect anywhere in the subtree).
math::Aabb node_influence_bound(const SdfContent& content, NodeId id, const Layer& layer);

// Whole-layer bound (union of root node bounds).
math::Aabb layer_influence_bound(const Layer& layer);

}  // namespace scene
}  // namespace clay
