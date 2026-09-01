#pragma once

// The layer digest, shared by everything in `session` that has to ask "is this
// still the thing I was looking at".
//
// PRIVATE to src/session: not installed, no ABI, and deliberately not part of
// the public header. What is public is the two questions it answers —
// `session::layer_fingerprint` (sdf_sculpt.h) and
// `session::layer_prefix_fingerprint` (sdf_prefix_cache.h) — because a caller
// wants an answer, not a hashing kit.
//
// FNV-1a over the bytes a caller could have changed. Chosen for being three
// lines rather than for its distribution: what is needed is "this is not what I
// started from", and the cost of a false MATCH is a stale reuse, which is what
// a full compare would cost a full compare to avoid.
//
// It exists in one place because there are now TWO consumers with the same
// correctness requirement and different scopes: a sculpt transaction digests a
// whole layer, and the prefix cache digests the first N roots. A second
// implementation that forgot a field would be a cache that silently served a
// stale prefix, which is the one failure mode neither can tolerate.

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "clay/math/transform.h"
#include "clay/scene/document.h"
#include "clay/scene/types.h"

namespace clay {
namespace session {
namespace digest {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

inline void mix_bytes(std::uint64_t& h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

// SCALARS AND ENUMS ONLY. A struct mixed through this would fold its PADDING
// in, and padding is not a value: two layers that are equal in every field
// could hash apart because one of them was built by a copy that left different
// bytes in the holes. Every aggregate below is mixed field by field for that
// reason, which is also why this refuses one.
template <typename T>
inline void mix(std::uint64_t& h, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "mix() is for flat data");
    static_assert(std::is_scalar<T>::value || std::is_enum<T>::value,
                  "mix() takes scalars: an aggregate would fold its padding in");
    mix_bytes(h, &value, sizeof(T));
}

// A float mixed by its BITS, so the digest cannot be fooled by a value that
// prints the same. -0.0f and 0.0f differ here and are different documents to
// nothing else; that costs a spurious conflict at worst, never a missed one.
inline void mix_f(std::uint64_t& h, float v) { mix(h, v); }

inline void mix_v3(std::uint64_t& h, kernel::cfloat3 v) {
    mix_f(h, v.x);
    mix_f(h, v.y);
    mix_f(h, v.z);
}

inline void mix_v2(std::uint64_t& h, kernel::cfloat2 v) {
    mix_f(h, v.x);
    mix_f(h, v.y);
}

inline void mix_transition(std::uint64_t& h, const scene::Transition& t) {
    mix_v3(h, t.a);
    mix_v3(h, t.b);
    mix_f(h, t.r0);
    mix_f(h, t.r1);
    mix(h, t.ease);
}

inline void mix_repeat(std::uint64_t& h, const scene::Repeat& r) {
    mix(h, r.type);
    mix_v3(h, r.spacing);
    mix_v3(h, r.counts);
}

inline void mix_profile(std::uint64_t& h, const scene::Profile& p) {
    mix(h, p.type);
    for (float v : p.params) mix_f(h, v);
}

inline void mix_xform(std::uint64_t& h, const math::Transform& t) {
    mix_v3(h, t.position);
    mix_f(h, t.rotation.x);
    mix_f(h, t.rotation.y);
    mix_f(h, t.rotation.z);
    mix_f(h, t.rotation.w);
    mix_f(h, t.scale);
}

inline void mix_points(std::uint64_t& h, const std::vector<scene::StrokePoint>& points) {
    mix(h, points.size());
    for (const scene::StrokePoint& p : points) {
        mix_v3(h, p.pos);
        mix_f(h, p.radius);
        mix(h, p.type);
        mix_v3(h, p.in_handle);
        mix_v3(h, p.out_handle);
    }
}

inline void mix_deformers(std::uint64_t& h, const std::vector<scene::Deformer>& chain) {
    mix(h, chain.size());
    for (const scene::Deformer& d : chain) {
        mix(h, d.type);
        mix_f(h, d.k);
        mix_f(h, d.a);
        mix_f(h, d.b);
        mix_f(h, d.c);
        mix(h, d.ease);
        for (float e : d.ext) mix_f(h, e);
        mix_points(h, d.guide);
        mix(h, d.cage.size());
        for (kernel::cfloat3 c : d.cage) mix_v3(h, c);
        mix_xform(h, d.cage_xform);
        // The stamp by its shape and its derived bounds rather than by its
        // samples: a 4K alpha is sixteen million floats and `refresh()` already
        // reduces the samples to the two numbers that describe them.
        mix(h, d.stamp.width);
        mix(h, d.stamp.height);
        mix_f(h, d.stamp.extent);
        mix_f(h, d.stamp.radius);
        mix_f(h, d.stamp.amplitude);
        mix_f(h, d.stamp.steepest);
        mix_f(h, d.stamp.peak);
    }
}

// A shared payload folded in by IDENTITY. These are held as
// `shared_ptr<const T>` and are immutable once shared, so a change is always a
// different object; hashing the samples would cost megabytes per check to
// learn what the address already says.
inline void mix_shared(std::uint64_t& h, const void* p) {
    const auto as_int = reinterpret_cast<std::uintptr_t>(p);
    mix(h, as_int);
}

inline void mix_node(std::uint64_t& h, const scene::SdfContent& content, scene::NodeId id);

inline void mix_children(std::uint64_t& h, const scene::SdfContent& content,
                  const std::vector<scene::NodeId>& ids) {
    mix(h, ids.size());
    for (scene::NodeId id : ids) mix_node(h, content, id);
}

inline void mix_node(std::uint64_t& h, const scene::SdfContent& content, scene::NodeId id) {
    mix(h, id);
    const scene::Node* n = content.find(id);
    if (!n) {
        // A dangling root is itself a state, and one worth telling apart from
        // the node being there.
        mix_bytes(h, "absent", 6);
        return;
    }
    mix(h, n->is_group);
    mix(h, n->visible);
    mix(h, n->op);
    mix(h, n->blend.profile);
    mix_f(h, n->blend.k);
    mix(h, n->prim.type);
    for (float p : n->prim.params) mix_f(h, p);
    mix_xform(h, n->xform);
    mix_v3(h, n->scale_axes);
    mix_f(h, n->rounding);
    mix_v3(h, n->color);
    mix(h, n->mirror);
    mix_points(h, n->stroke);
    mix_f(h, n->stroke_blend_k);
    mix(h, n->stroke_closed);
    mix_f(h, n->curve_tolerance);
    mix(h, n->armature_parents.size());
    for (std::uint32_t p : n->armature_parents) mix(h, p);
    mix(h, n->armature_signs.size());
    for (std::int8_t s : n->armature_signs) mix(h, s);
    mix_deformers(h, n->deformers);
    mix_transition(h, n->transition);
    mix_repeat(h, n->repeat);
    mix_profile(h, n->profile);
    mix(h, n->profile_points.size());
    for (kernel::cfloat2 p : n->profile_points) mix_v2(h, p);
    mix(h, n->profiles.size());
    for (const scene::Profile& p : n->profiles) mix_profile(h, p);
    mix(h, n->profile_polygons.size());
    for (const auto& poly : n->profile_polygons) {
        mix(h, poly.size());
        for (kernel::cfloat2 p : poly) mix_v2(h, p);
    }
    mix_shared(h, n->volume.get());
    if (n->volume) mix(h, n->volume->sample_count());
    mix_shared(h, n->gate.get());
    mix_f(h, n->gate_width);
    mix_children(h, content, n->children);
}

// The layer's OWN properties — everything about a layer that is not a root.
// Shared so that a whole-layer digest and a prefix digest cannot disagree
// about what a layer is; a prefix built under one mirror and reused under
// another is a different field.
inline void mix_layer_head(std::uint64_t& h, const scene::Layer& layer) {
    mix(h, layer.id);
    mix(h, layer.kind);
    mix(h, layer.name.size());
    mix_bytes(h, layer.name.data(), layer.name.size());
    mix_xform(h, layer.xform);
    mix(h, layer.scale_axes.x);  // the per-axis scale is part of the placement
    mix(h, layer.scale_axes.y);
    mix(h, layer.scale_axes.z);
    mix(h, layer.visible);
    mix(h, layer.ghost);
    mix(h, layer.locked);
    mix(h, layer.resolution);
    mix(h, layer.mirror_axes);
    mix_f(h, layer.mirror_k);
    mix(h, layer.radial_count);
    mix(h, layer.radial_axis);
    mix_f(h, layer.radial_k);
}

// The first `count` roots, and NOTHING after them.
//
// The count is mixed in as well as bounding the walk, so a prefix of three
// roots and a prefix of four that happen to agree on the first three are
// still different digests — a boundary is part of what is being identified.
//
// The CONTENT, not the pointer: an instance layer shares its edit list, so an
// edit through a sibling is an edit here and the shared address would not have
// moved.
inline void mix_roots(std::uint64_t& h, const scene::Layer& layer, std::size_t count) {
    mix(h, count);
    if (!layer.sdf) return;
    const std::vector<scene::NodeId>& roots = layer.sdf->roots;
    const std::size_t n = count < roots.size() ? count : roots.size();
    for (std::size_t i = 0; i < n; ++i) mix_node(h, *layer.sdf, roots[i]);
}

}  // namespace digest
}  // namespace session
}  // namespace clay
