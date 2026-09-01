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

// WHY an item's influence is not its own geometry — the three answers, split
// out because two of them used to share one (#319).
enum class Nonlocality : std::uint8_t {
    None,            // the item's own geometry bounds it
    BoundedByLayer,  // an INTERSECT: it can only take material away, and what
                     // it takes away is inside what the layer already occupies
    Unbounded,       // a spatial morph, an infinite grid repeat, an unbounded
                     // primitive: no finite box holds it
};
Nonlocality item_nonlocality(const Node& item);

// The union of a layer's visible item geometry, and infinite the moment one of
// them has none — the box an intersect is bounded by. An intersect in a layer
// holding a plane is bounded by a plane.
math::Aabb layer_influence_extent(const SdfContent& content, const Layer& layer);

// World-space INFLUENCE bound: the geometry bound for local ops, the LAYER's
// extent for an intersect, and infinite for what genuinely has none — a
// spatial morph, an infinite grid repeat, an unbounded primitive.
//
// The intersect case is what #319 asked for and #326 measured. `max(acc, item)`
// cannot put material where the layer has none, so the layer bounds it: over
// 400,000 sample points on two fixtures the band-clamped drift outside the
// layer's extent is exactly 0, against 0.100 and 0.065 outside the item's own
// geometry — which looks like the tighter answer and does not hold. A spatial
// morph's weight SATURATES, so past its span the result is the item's own
// field arbitrarily far away, and it keeps the infinite answer (measured:
// 0.0157 of drift outside the layer extent for the radial morph).
//
// This is the DIRTY bound. It is not the cull gate: `item_influence_is_local`
// stays false for every non-local op, so per-brick culling still cannot drop
// an intersect from a tape.
//
// `layer_extent` is an optimisation, not a parameter with meaning: pass the
// layer's extent if you already hold it and this will not recompute it. Only
// an intersect reads it, so a layer without one pays nothing either way.
math::Aabb item_influence_bound(const Node& item, const Layer& layer,
                                const math::Aabb* layer_extent = nullptr);

// The item ALONE, as the layer places it: the influence bound without the
// reflected and rotated copies the layer's symmetry emits and without the
// seam dilation they bring. Same rounding and combine dilations, infinite for
// the same non-local items.
//
// NOT given the intersect fallback above: this answers "how far does this
// item's own body reach", and for a non-local item the honest answer to THAT
// is still everywhere — the caller is deciding whether to warp the item at
// all, not which bricks to redraw.
//
// This is what a brush that has already reflected ITSELF tests against
// (brush/move.cpp): under a mirror every participating item's
// item_influence_bound spans the plane, so a ball on one side reaches every
// item on both, and a warp aimed at the ball is a no-op for the ones whose
// body sits on the far side. Culling and invalidation keep
// item_influence_bound — the copies are real geometry there, and its
// every-copy contract is untouched.
math::Aabb item_own_influence_bound(const Node& item, const Layer& layer);

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

// The chain pad's length-aware envelope, in K-MULTIPLES: how many k of pad a
// smooth distance blend of this profile needs in a chain of `nodes`
// CONTRIBUTORS. Grows with the count because the drag a chain accumulates
// grows with its length (see blend_cull_pad's definition); the caller clamps
// the product `k * envelope` at the profile's support, which is the pre-#335
// pad, so the result never exceeds it. Only the three measured smooth
// distance profiles have a fit; everything else takes the clamp.
//
// `nodes` is the EFFECTIVE contributor count, not the node-map size: a layer
// with mirror or radial symmetry compiles every mirrored item once per copy
// (tape_build.cpp, emit_item), each copy a real leaf entering the layer's one
// serial chain through its own seam combine, so the chain is
// layer_symmetry_multiplicity times as long as the map says. Resolving the
// envelope against the map size alone measured real in-band disagreements at
// radial_count 8-64 — the amplified knees match plain chains of the same
// effective length.
float chain_pad_envelope(BlendProfile profile, std::size_t nodes);

// How many instances of a mirrored item this layer's symmetry emits: 1 for
// the item, one per set mirror axis, and radial_count - 1 rotated copies —
// the modes compose ADDITIVELY (tape_build.cpp emit_item copies the BASE
// item; no mirror-of-rotation products are emitted). Multiplying the node
// count by this over-counts items that opted out of the mirror, groups and
// feathered replaces, which is safe: the chain-pad envelope is monotone and
// every per-item term stays clamped at its own support.
std::size_t layer_symmetry_multiplicity(const Layer& layer);

// How far this node can drag a CHAIN's running value, which is the quantity the
// pad below is the maximum of — and NOT the same question as how far the node's
// own combine reaches, which is `max(support, k)` and stays that way.
//
// A HARD profile drags nothing: `ctape_smin_m` hands it back a step, so the
// running value it produces is `min()` of two operands and moves by no `k` at
// all. `Paint` and the extended modes still drag — paint's colour fades over
// `max(support, k)` whatever the profile, and the extended modes ignore the
// profile by design — so they keep the full reach.
//
// A `k` left on a hard node, which is what a UI that keeps its blend slider
// when the artist picks "hard" writes, therefore used to set the pad for the
// WHOLE LAYER out of a blend that drags nothing (#335).
//
// An ordinary smooth distance blend returns `min(support, k * envelope(N))`
// (#335), where N is the layer's EFFECTIVE contributor count — node-map size
// times layer_symmetry_multiplicity, the conservative stand-in for the chain
// length the drag actually grows with, and the reason this takes the count:
// one node cannot know how long a chain it sits in. The envelope is a
// K-MULTIPLE, not a support fraction, because the measured sufficient pads
// cluster in k-units across every profile — the drag a step adds is normalized
// to k, support only sets the fringe's shape. It RISES WITH N because the
// measured knees do: the quadratic knee, worst order per length, measured
// 2.30k at 75 nodes, 3.05k at 600, 3.45k at 1200 and 3.90k at 5000
// (blend_cull_pad's definition records the campaign). The fit must clear
// EVERY measured knee with at least the 0.5k the knees drift across seed
// draws; changing it needs that sweep's breadth of evidence, not a tuning
// pass.
//
// DO NOT reuse this for a node's own bound. That bound's `max(support, k)`
// dilation is doing a second job in a mixed chain — margin for the drag its
// SMOOTH neighbours apply to a running value it contributed to — and taking it
// away measured 540 -> 10,105 band-clamped disagreements on a 12-node
// hard/smooth document, against 0 for narrowing the pad alone.
float chain_drag_reach(const Node& item, std::size_t effective_nodes);

// The pad a smooth-union CHAIN needs beyond the caller's band. An item's own
// bound covers what ONE blend can move; a chain's running value sits above its
// final one, so an item can steer it from further out than its own support.
// See the definition for the measurements.
float blend_cull_pad(const SdfContent& content, const Layer& layer);

// The pad's terms, UNADDED and UNRESOLVED. Kept apart because the document's
// pad is a MAXIMUM OF SUMS over its layers: adding a layer's resolved terms
// gives that layer's sum, which is what `cull_pad` returns, but folding two
// layers' terms together and adding at the end would give a SUM OF MAXIMA --
// larger, so safe, and no longer the number a fresh build reports. A caller
// that wants to raise a pad incrementally (scene/cull_index.h) therefore
// holds these PER LAYER.
//
// The blend term is held as RAW MAXIMA (largest k per profile, largest
// N-independent reach) rather than as a folded pad, because the chain pad
// depends on the layer's node count and the count is not known until READ
// time. Folding `min(support, k * envelope(N))` per node at gather time would
// freeze each node's N at whatever the map held when it was gathered: an
// append that grows the map would leave the old nodes' folded contributions
// below what a fresh build reports, breaking the raise-only incremental
// contract cull_index.cpp relies on. Raw maxima and the count only rise, and
// the envelope rises with the count, so resolving at read time keeps an
// appended index equal to a rebuilt one. The symmetry multiplicity is read
// at resolve time too, from the live layer — a symmetry edit is never an
// append (cull_index.cpp, refresh_pad), so it cannot go stale between the
// two.
struct CullPadTerms {
    float feather = 0.0f;
    // N-independent reaches: Paint/extended full reach, and the support of any
    // profile the envelope does not narrow (chamfer's support is its k).
    float blend_fixed = 0.0f;
    // Largest k per measured smooth distance profile. Per profile the support
    // is linear in k, so min(support(k_max), k_max * envelope(N)) IS the
    // largest per-node pad of that profile — the fold is exact, not merely
    // conservative.
    float blend_k_quadratic = 0.0f;
    float blend_k_cubic = 0.0f;
    float blend_k_circular = 0.0f;
    // Largest SEAM k among the layer's mirrored items: the blends the layer's
    // mirror/radial copies enter the chain through (tape_build.cpp emit_item —
    // both seams are quadratic with the LAYER's k, independent of any item k).
    // A NEW slot, not folded into blend_k_quadratic: the item slots stay pure
    // so the pre-#335 pad remains derivable from them alone as the ceiling
    // blend_total clamps the seam term to.
    float blend_k_seam = 0.0f;

    void raise(const CullPadTerms& o) {
        feather = kernel::cmax(feather, o.feather);
        blend_fixed = kernel::cmax(blend_fixed, o.blend_fixed);
        blend_k_quadratic = kernel::cmax(blend_k_quadratic, o.blend_k_quadratic);
        blend_k_cubic = kernel::cmax(blend_k_cubic, o.blend_k_cubic);
        blend_k_circular = kernel::cmax(blend_k_circular, o.blend_k_circular);
        blend_k_seam = kernel::cmax(blend_k_seam, o.blend_k_seam);
    }
    // The blend term resolved for a layer whose chain holds `n_eff` EFFECTIVE
    // contributors — node-map size times layer_symmetry_multiplicity, both
    // read at resolve time so a symmetry edit and an append flow through the
    // same refresh. ONE layer-wide value, so the seed keying the C ABI gates
    // on a single pad float stays valid (#362).
    float blend_total(std::size_t n_eff) const;
    float total(std::size_t n_eff) const { return feather + blend_total(n_eff); }
};

// One node's contribution, so a caller that has GAINED a node can raise a
// layer's terms from it alone instead of walking the layer again -- what makes
// extending a cull index cost the dab rather than the document (#347). An
// invisible node contributes nothing, exactly as the walk skips it.
CullPadTerms cull_pad_terms(const Node& node, const Layer& layer);

// A whole layer's terms, in ONE walk of the node map.
CullPadTerms cull_pad_terms(const SdfContent& content, const Layer& layer);

// Both of the above, in ONE walk of the node map. What a compile actually
// wants: each of them walks every node, and at ten thousand items the second
// walk measured 20-30% on the per-brick cull benchmarks.
float cull_pad(const SdfContent& content, const Layer& layer);

// Influence bound of any node (recursive union for groups, dilated by the
// group's blend support; infinite for intersect anywhere in the subtree).
math::Aabb node_influence_bound(const SdfContent& content, NodeId id, const Layer& layer);

// How far a GROUP's combine spreads a change in one of its operands. Shared by
// node_influence_bound, which applies it to the union of the children, and
// node_reach_bound, which applies it to one child — two spellings of the same
// quantity would be one refactor away from disagreeing.
float group_blend_support(const Node& group, const Layer& layer);

// Where an edit to `id` can change the layer's field: node_influence_bound,
// dilated once per enclosing group by that group's blend support, up to the
// root.
//
// This is the answer to "where does an edit to this node LAND", which is a
// different question from "where is this node" and used to be answered with
// the root ancestor's whole bound. It is conservative in the same band-clamped
// sense every bound here is, and it does NOT grow with the size of the group:
// a sibling's geometry is not something an edit to `id` can reach.
//
// Infinite when the node is non-local or any group above it is; empty when the
// node, or any group above it, is hidden or absent.
math::Aabb node_reach_bound(const SdfContent& content, NodeId id, const Layer& layer);

// Whole-layer bound (union of root node bounds).
// The box outside which this node cannot change the DOCUMENT's field: the union
// over every visible layer sharing its content, since an instanced layer
// compiles the same node again under its own transform. Prefer this to
// node_influence_bound wherever a Document is in scope and the answer is going
// to a host as a region to dirty (issue #325).
math::Aabb node_influence_bound_in_document(const Document& doc, const SdfContent& content,
                                            NodeId id);

math::Aabb layer_influence_bound(const Layer& layer);

}  // namespace scene
}  // namespace clay
