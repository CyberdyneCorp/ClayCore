// Transient SDF sculpt transactions (sdf-sculpt-transaction spec). See
// include/clay/session/sdf_sculpt.h for why field and deformation brushes need
// a lifetime that an edit-list brush does not, and why the state lives here.

#include "clay/session/sdf_sculpt.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "clay/scene/bounds.h"

namespace clay {
namespace session {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// -- the fingerprint ---------------------------------------------------------
//
// FNV-1a over the bytes a caller could have changed. Chosen for being three
// lines rather than for its distribution: what is needed is "this is not the
// layer I started from", and the cost of a false MATCH is a stale commit,
// which is what a full compare would cost a full compare to avoid.

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix_bytes(std::uint64_t& h, const void* data, std::size_t size) {
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
void mix(std::uint64_t& h, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "mix() is for flat data");
    static_assert(std::is_scalar<T>::value || std::is_enum<T>::value,
                  "mix() takes scalars: an aggregate would fold its padding in");
    mix_bytes(h, &value, sizeof(T));
}

// A float mixed by its BITS, so the digest cannot be fooled by a value that
// prints the same. -0.0f and 0.0f differ here and are different documents to
// nothing else; that costs a spurious conflict at worst, never a missed one.
void mix_f(std::uint64_t& h, float v) { mix(h, v); }

void mix_v3(std::uint64_t& h, cfloat3 v) {
    mix_f(h, v.x);
    mix_f(h, v.y);
    mix_f(h, v.z);
}

void mix_v2(std::uint64_t& h, kernel::cfloat2 v) {
    mix_f(h, v.x);
    mix_f(h, v.y);
}

void mix_transition(std::uint64_t& h, const scene::Transition& t) {
    mix_v3(h, t.a);
    mix_v3(h, t.b);
    mix_f(h, t.r0);
    mix_f(h, t.r1);
    mix(h, t.ease);
}

void mix_repeat(std::uint64_t& h, const scene::Repeat& r) {
    mix(h, r.type);
    mix_v3(h, r.spacing);
    mix_v3(h, r.counts);
}

void mix_profile(std::uint64_t& h, const scene::Profile& p) {
    mix(h, p.type);
    for (float v : p.params) mix_f(h, v);
}

void mix_xform(std::uint64_t& h, const math::Transform& t) {
    mix_v3(h, t.position);
    mix_f(h, t.rotation.x);
    mix_f(h, t.rotation.y);
    mix_f(h, t.rotation.z);
    mix_f(h, t.rotation.w);
    mix_f(h, t.scale);
}

void mix_points(std::uint64_t& h, const std::vector<scene::StrokePoint>& points) {
    mix(h, points.size());
    for (const scene::StrokePoint& p : points) {
        mix_v3(h, p.pos);
        mix_f(h, p.radius);
        mix(h, p.type);
        mix_v3(h, p.in_handle);
        mix_v3(h, p.out_handle);
    }
}

void mix_deformers(std::uint64_t& h, const std::vector<scene::Deformer>& chain) {
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
        for (cfloat3 c : d.cage) mix_v3(h, c);
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
void mix_shared(std::uint64_t& h, const void* p) {
    const auto as_int = reinterpret_cast<std::uintptr_t>(p);
    mix(h, as_int);
}

void mix_node(std::uint64_t& h, const scene::SdfContent& content, scene::NodeId id);

void mix_children(std::uint64_t& h, const scene::SdfContent& content,
                  const std::vector<scene::NodeId>& ids) {
    mix(h, ids.size());
    for (scene::NodeId id : ids) mix_node(h, content, id);
}

void mix_node(std::uint64_t& h, const scene::SdfContent& content, scene::NodeId id) {
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

// -- shared transaction plumbing ---------------------------------------------

// The layer, if it is one this file may still act on. A layer that was removed
// or protected since begin is as much a reason to refuse a commit as one that
// was edited.
const scene::Layer* editable_layer(const scene::Document& doc, scene::LayerId id) {
    const scene::Layer* layer = doc.find_layer(id);
    if (!layer || layer->kind != scene::LayerKind::Sdf || !layer->sdf) return nullptr;
    if (layer->protected_from_edits()) return nullptr;
    return layer;
}

// The three sampling numbers, with ConsolidationParams' defaults applied once
// so Smooth's working volume and any later consolidation of it agree.
scene::ConsolidationParams params_from(const SdfSculptPolicy& policy) {
    scene::ConsolidationParams params;
    params.cell_size = policy.cell_size;
    params.band = policy.band;
    params.padding = policy.padding;
    return params;
}

// What a policy-triggered consolidation should resample at. Its own numbers
// when it has them, and otherwise the ones the gesture is already using —
// which is the resolution the host chose for this sculpt mode, and the only
// one available that nobody has to guess.
scene::ConsolidationParams consolidation_params_from(const SdfSculptPolicy& policy) {
    scene::ConsolidationParams params = policy.complexity.consolidation;
    if (!(params.cell_size > 0.0f)) {
        params.cell_size = policy.cell_size;
        params.band = policy.band;
        params.padding = policy.padding;
    }
    return params;
}

// The whole post-stroke half, run INSIDE the caller's open undo group so that
// the stroke and anything it triggers are one thing to undo.
//
// Consolidation is skipped on a layer that is already a single volume item.
// Baking one of those again would resample samples into samples for no change
// in what the layer costs — consolidate.h's `consolidation_state` is exactly
// the question, and Smooth's own commit leaves the layer in that state.
SdfSculptBudget settle_budget(scene::Document& doc, scene::LayerId layer_id,
                              const SdfSculptPolicy& policy, const scene::BakePointEval& eval,
                              scene::UndoStack* undo, bool may_consolidate) {
    SdfSculptBudget out;
    const scene::Layer* layer = doc.find_layer(layer_id);
    if (!layer) return out;
    // MEASURED either way: a host wants the numbers whether or not it has
    // authorised anything to be done about them.
    out.report = scene::report_layer(*layer, policy.complexity.min_safe_step_scale);
    out.over_budget = over_sculpt_budget(policy.complexity, out.report);
    if (!out.over_budget || !policy.complexity.allow_consolidation) return out;
    // A gesture that applied NO command is not a completed stroke, and a bake
    // triggered by one would be a destructive edit the artist did not make: a
    // drag over empty space would silently collapse the layer.
    if (!may_consolidate) return out;
    if (scene::consolidation_state(*layer, &out.cost)) return out;  // nothing left to collapse
    out.consolidated = scene::consolidate_layer(doc, layer_id, consolidation_params_from(policy),
                                                undo, &out.cost, eval);
    if (out.consolidated) {
        const scene::Layer* after = doc.find_layer(layer_id);
        if (after) out.report = scene::report_layer(*after, policy.complexity.min_safe_step_scale);
    }
    return out;
}

}  // namespace

std::uint64_t layer_fingerprint(const scene::Layer& layer) {
    std::uint64_t h = kFnvOffset;
    mix(h, layer.id);
    mix(h, layer.kind);
    mix(h, layer.name.size());
    mix_bytes(h, layer.name.data(), layer.name.size());
    mix_xform(h, layer.xform);
    mix(h, layer.visible);
    mix(h, layer.ghost);
    mix(h, layer.locked);
    mix(h, layer.resolution);
    mix(h, layer.mirror_axes);
    mix_f(h, layer.mirror_k);
    mix(h, layer.radial_count);
    mix(h, layer.radial_axis);
    mix_f(h, layer.radial_k);
    // The CONTENT, not the pointer. An instance layer shares its edit list, so
    // an edit through a sibling instance is an edit to this layer and the
    // shared address would not have moved.
    if (layer.sdf) mix_children(h, *layer.sdf, layer.sdf->roots);
    return h;
}

bool over_sculpt_budget(const SdfSculptComplexityPolicy& policy,
                        const scene::FieldReport& report) {
    // Zero disables a criterion, so an empty policy is never over budget.
    if (policy.min_safe_step_scale > 0.0f && report.safe_step_scale < policy.min_safe_step_scale)
        return true;
    if (policy.max_deformer_chain > 0 && report.longest_deformer_chain > policy.max_deformer_chain)
        return true;
    if (policy.max_item_count > 0 && report.item_count > policy.max_item_count) return true;
    return false;
}

// -- Smooth ------------------------------------------------------------------

std::optional<SdfSmoothTransaction> SdfSmoothTransaction::begin(
    scene::Document& doc, scene::LayerId layer_id, const SdfSculptPolicy& policy,
    const scene::BakePointEval& point_eval, parallel::CancelToken* token) {
    const scene::Layer* layer = editable_layer(doc, layer_id);
    if (!layer) return std::nullopt;
    if (!(policy.cell_size > 0.0f)) return std::nullopt;

    // THE ONLY evaluation of the source layer in the whole gesture. Through
    // bake_layer, so the sampling is the one consolidation already does —
    // local frame, pooled evaluator, redistance, compact, measured Lipschitz —
    // and a Smooth commit installs something a bake could have produced.
    std::optional<field::FieldVolume> volume =
        scene::bake_layer(*layer, params_from(policy), nullptr, point_eval, token);
    if (!volume) return std::nullopt;

    SdfSmoothTransaction tx;
    tx.doc_ = &doc;
    tx.layer_ = layer_id;
    tx.policy_ = policy;
    tx.point_eval_ = point_eval;
    tx.source_ = layer_fingerprint(*layer);
    tx.working_ = std::move(*volume);
    return tx;
}

SdfSculptDirty SdfSmoothTransaction::update(const field::RelaxSettings& settings,
                                            parallel::CancelToken* token) {
    SdfSculptDirty dirty;
    if (!live()) return dirty;
    // The whole update, and deliberately all of it: no compile, no bake, no
    // command, no undo entry. The working volume is already the layer's field.
    const field::RelaxResult r = field::relax_in_place(working_, settings, token);
    dirty.bounds = r.dirty_bounds;
    dirty.touched_bricks = r.touched_bricks;
    dirty.changed = r.changed;
    return dirty;
}

bool SdfSmoothTransaction::commit(scene::UndoStack* undo) {
    if (!live()) return false;
    scene::Document& doc = *doc_;
    const scene::Layer* layer = editable_layer(doc, layer_);
    // Refused rather than forced. The preview was computed from a layer that no
    // longer exists, and installing it would delete whatever replaced it
    // without the artist having asked for that.
    if (!layer || layer_fingerprint(*layer) != source_) {
        cancel();
        return false;
    }

    // ONE user-facing step, opened here and closed once, with the install's own
    // bracket and any consolidation's bracket nesting inside it — see
    // UndoStack::begin_group.
    if (undo) undo->begin_group();
    scene::ConsolidationCost installed_cost;
    const bool installed = scene::replace_layer_with_volume(doc, layer_, std::move(working_),
                                                            undo, &installed_cost);
    if (installed) {
        budget_ = settle_budget(doc, layer_, policy_, point_eval_, undo, /*may_consolidate=*/true);
        // The installed volume's cost unless a consolidation replaced it, in
        // which case the consolidation's is what the layer now holds.
        if (!budget_.consolidated) budget_.cost = installed_cost;
    }
    if (undo) undo->end_group();

    doc_ = nullptr;
    return installed;
}

void SdfSmoothTransaction::cancel() {
    // Nothing persistent was ever written, so there is nothing to unwind. The
    // working volume is released because a cancelled preview is not a thing a
    // host should still be able to draw.
    working_ = field::FieldVolume();
    doc_ = nullptr;
}

// -- Move --------------------------------------------------------------------

std::optional<SdfMoveTransaction> SdfMoveTransaction::begin(scene::Document& doc,
                                                            scene::LayerId layer_id,
                                                            cfloat3 world_centre,
                                                            const brush::MoveSettings& settings,
                                                            const SdfSculptPolicy& policy,
                                                            const scene::BakePointEval& point_eval) {
    const scene::Layer* layer = editable_layer(doc, layer_id);
    if (!layer) return std::nullopt;
    if (!(settings.radius > 0.0f)) return std::nullopt;  // not a drag

    SdfMoveTransaction tx;
    tx.doc_ = &doc;
    tx.layer_ = layer_id;
    tx.policy_ = policy;
    tx.point_eval_ = point_eval;
    tx.settings_ = settings;
    tx.anchor_ = world_centre;
    tx.source_ = layer_fingerprint(*layer);

    // THE ONLY traversal of the edit list in the whole gesture. The anchor and
    // the radius are fixed for a drag, so which items it reaches and where its
    // centre lands in each of their frames cannot change; only the
    // displacement does, and that is `resolve_prepared_move`.
    const std::vector<brush::PreparedMove> prepared =
        brush::prepare_move(*layer, world_centre, settings, &tx.prepare_stats_);
    tx.affected_.reserve(prepared.size());
    for (const brush::PreparedMove& p : prepared) {
        const scene::Node* n = layer->sdf->find(p.node);
        if (!n) continue;
        Affected a;
        a.id = p.node;
        a.prepared = p;
        // The PRE-STROKE chain, kept by value. Every frame's preview and the
        // final commit are built from this and nothing else, which is what
        // makes a preview and its commit the same thing and what makes an
        // update of 0.5 mean 0.5 rather than 0.1 + 0.2 + 0.5.
        a.original_chain = n->deformers;
        a.preview_chain = n->deformers;
        tx.affected_ids_.push_back(p.node);
        tx.affected_.push_back(std::move(a));
    }

    // A private copy of the layer, with its own edit list. The preview is
    // ordinary scene content, so the host compiles, draws and picks it through
    // the paths it already has instead of through an overlay the tape compiler
    // would have to learn about.
    tx.preview_ = *layer;
    tx.preview_.sdf = std::make_shared<scene::SdfContent>(*layer->sdf);
    return tx;
}

SdfSculptDirty SdfMoveTransaction::update(cfloat3 total_world_displacement) {
    SdfSculptDirty dirty;
    if (!live()) return dirty;
    displacement_ = total_world_displacement;
    last_update_visited_ = 0;

    for (Affected& a : affected_) {
        ++last_update_visited_;
        const brush::MoveWarp warp =
            brush::resolve_prepared_move(a.prepared, total_world_displacement);
        // Against the ORIGINAL chain, through the one function that owns the
        // ordering rule. A grab goes at the FRONT, and one already leading from
        // this same drag is replaced rather than stacked on.
        a.preview_chain = brush::moved_chain(a.original_chain, warp);
        if (scene::Node* n = preview_.sdf->find_mut(a.id)) n->deformers = a.preview_chain;
    }

    // Conservative and swept: the ball the drag started in, united with the
    // ball it has reached. A grab moves nothing outside its own support, and
    // its support travels with the displacement, so this covers both where the
    // surface was and where it went. Tighter is possible and not worth being
    // wrong about — a dirty region that is too small leaves stale pixels, and a
    // preview that lies is worse than one that redraws a little extra.
    const cfloat3 r = cf3(settings_.radius, settings_.radius, settings_.radius);
    dirty.bounds.expand(math::Aabb{anchor_ - r, anchor_ + r});
    const cfloat3 moved = anchor_ + total_world_displacement;
    dirty.bounds.expand(math::Aabb{moved - r, moved + r});
    dirty.touched_bricks = affected_.size();
    dirty.changed = !affected_.empty() && kernel::clength(total_world_displacement) > 0.0f;
    return dirty;
}

bool SdfMoveTransaction::commit(scene::UndoStack* undo) {
    if (!live()) return false;
    scene::Document& doc = *doc_;
    const scene::Layer* layer = editable_layer(doc, layer_);
    if (!layer || layer_fingerprint(*layer) != source_) {
        cancel();
        return false;
    }

    // Rebuilt from the captured originals rather than trusted from the preview,
    // so a commit is correct even if the host never called update — and so the
    // two can never be computed by different code.
    if (undo) undo->begin_group();
    bool ok = true;
    std::size_t applied = 0;
    if (kernel::clength(displacement_) > 0.0f) {
        for (const Affected& a : affected_) {
            const brush::MoveWarp warp = brush::resolve_prepared_move(a.prepared, displacement_);
            scene::SetDeformersCmd cmd;
            cmd.layer = layer_;
            cmd.node = a.id;
            cmd.deformers = brush::moved_chain(a.original_chain, warp);
            const scene::Command command{std::move(cmd)};
            const bool applied_one =
                undo ? undo->perform(doc, command) : scene::apply(doc, command).has_value();
            ok = ok && applied_one;
            if (applied_one) ++applied;
        }
    }
    if (ok) budget_ = settle_budget(doc, layer_, policy_, point_eval_, undo, applied > 0);
    if (undo) undo->end_group();

    doc_ = nullptr;
    return ok;
}

bool SdfMoveTransaction::preview_grab(scene::NodeId node, scene::Deformer* out) const {
    for (const Affected& a : affected_) {
        if (a.id != node) continue;
        // Resolved fresh from the prepared candidate rather than read off the
        // preview chain: the two are the same warp, and taking it from the
        // resolver means a host and a commit cannot see different numbers.
        if (out) *out = brush::resolve_prepared_move(a.prepared, displacement_).deformer;
        return true;
    }
    return false;
}

void SdfMoveTransaction::cancel() {
    preview_ = scene::Layer{};
    affected_.clear();
    affected_ids_.clear();
    doc_ = nullptr;
}

}  // namespace session
}  // namespace clay
