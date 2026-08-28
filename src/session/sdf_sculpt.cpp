// Transient SDF sculpt transactions (sdf-sculpt-transaction spec). See
// include/clay/session/sdf_sculpt.h for why field and deformation brushes need
// a lifetime that an edit-list brush does not, and why the state lives here.

#include "clay/session/sdf_sculpt.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "clay/field/redistance.h"
#include "clay/scene/bounds.h"

#include "layer_digest.h"

namespace clay {
namespace session {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// The layer digest moved to src/session/layer_digest.h when the prefix cache
// grew a second consumer with the same correctness requirement and a
// different scope. One implementation, because one that forgot a field
// would be a cache serving a stale prefix.


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

// Whether a bake would change what this layer COSTS, or only resample it.
//
// A single volume item with nothing on it is already the thing a bake produces:
// baking it again reads samples and writes samples, and since a resample of a
// resample is measurably steeper (consolidate.h opens on exactly that), it
// would make the layer worse rather than better. Smooth's own commit leaves the
// layer in that state, so it must not immediately re-bake itself.
//
// A single volume item WITH A DEFORMER CHAIN is not that case, and the
// difference is the whole point. Consolidating turns a layer into one volume
// node, which makes `consolidation_state` true FOREVER AFTER — so a test on
// that predicate alone fires the policy once and then never again, while every
// later drag stacks another grab on the volume item and the safe step decays
// exactly as it did before. Measured: a hundred drags with the policy on ended
// at a chain of 58 and 0.221, one consolidation, instead of being held near the
// threshold. A bake absorbs those grabs into the samples, so where there is a
// chain there is something to collapse.
bool nothing_left_to_collapse(const scene::Layer& layer, const scene::FieldReport& report,
                              scene::ConsolidationCost* out_cost) {
    return scene::consolidation_state(layer, out_cost) && report.longest_deformer_chain == 0;
}

// The whole post-stroke half, run INSIDE the caller's open undo group so that
// the stroke and anything it triggers are one thing to undo.
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
    if (nothing_left_to_collapse(*layer, out.report, &out.cost)) return out;
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
    // The whole layer: its own properties, then every root. Equal by
    // construction to `layer_prefix_fingerprint(layer, roots.size())`, which
    // test_sdf_prefix_cache.cpp pins so the two cannot drift.
    std::uint64_t h = digest::kFnvOffset;
    digest::mix_layer_head(h, layer);
    digest::mix_roots(h, layer, layer.sdf ? layer.sdf->roots.size() : 0);
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
    const scene::BakePointEval& point_eval, parallel::CancelToken* token,
    SdfPrefixCache* cache) {
    const scene::Layer* layer = editable_layer(doc, layer_id);
    if (!layer) return std::nullopt;
    if (!(policy.cell_size > 0.0f)) return std::nullopt;

    // NOTHING IS SAMPLED HERE. The source compiles the layer and looks for a
    // cached prefix; it never bakes. What used to happen at this line was a
    // whole-layer bake — the model's cost, paid at the one moment in a gesture
    // when the artist is already waiting.
    SdfPrefixPolicy prefix_policy = policy.prefix;
    prefix_policy.cell_size = policy.cell_size;
    prefix_policy.band = policy.band;
    prefix_policy.padding = policy.padding;
    std::optional<SdfSourceField> source =
        SdfSourceField::open(doc, layer_id, cache, prefix_policy, point_eval, token);
    if (!source) return std::nullopt;
    // Refused on the same terms bake_layer refuses: a layer with no field has
    // nothing to smooth, and a lattice cannot be derived from unbounded bounds.
    const math::Aabb bounds = source->bounds();
    if (bounds.empty() || bounds.is_infinite()) return std::nullopt;

    const float band = policy.band > 0.0f ? policy.band : policy.cell_size * 3.0f;
    const float padding = policy.padding > 0.0f ? policy.padding : band;
    const kernel::cfloat3 pad = cf3(padding, padding, padding);
    // THE LATTICE, and the one both the working field and the commit assembly
    // use — they must share it, because the overlay at commit is per sample and
    // `sample_blocks` takes its origin straight from `region.min`. It is also
    // the region a prefix is cached over, so a seed read here is the stored
    // number rather than an interpolation of two.
    const math::Aabb region{bounds.min - pad, bounds.max + pad};

    SdfSmoothTransaction tx;
    tx.doc_ = &doc;
    tx.layer_ = layer_id;
    tx.policy_ = policy;
    tx.point_eval_ = point_eval;
    tx.fingerprint_ = layer_fingerprint(*layer);
    tx.region_ = region;
    // An index and a far bound per brick, and no samples: the whole allocation
    // is the lattice, and every brick reads as sample-free until a dab asks.
    tx.working_ = field::FieldVolume::empty_lattice(region, policy.cell_size, band);
    tx.field_source_ = std::move(*source);
    return tx;
}

field::FieldVolume::Region SdfSmoothTransaction::dependency_region(
    const field::RelaxSettings& settings) const {
    const float cell = working_.cell_size();
    const int radius = std::max(1, settings.radius_cells);
    // relax's own widening, reproduced rather than guessed: a falloff narrower
    // than the kernel cannot hide the seam the kernel makes, so relax silently
    // widens it and the region it rewrites is the widened one.
    const float falloff = std::max(settings.falloff, cell * static_cast<float>(radius) * 2.0f);
    if (!(settings.region_radius > 0.0f)) {
        // A radius of zero means EVERYWHERE — a filter rather than a brush —
        // and there is nothing local about it. The whole lattice is the
        // dependency, which is the eager cost, correctly.
        return field::FieldVolume::Region{region_};
    }
    // What relax rewrites, plus what its stencil READS from outside that.
    //
    // The rewrite is every brick whose BOX meets the ball, so a written sample
    // can sit a whole brick DIAGONAL beyond the ball's surface — not a brick
    // edge, which is the mistake this had first. From there the stencil reaches
    // `radius` cells further. A tap landing in a brick nobody materialized does
    // not read a wrong number, it reads NOTHING: `sample_at` returns nothing
    // for an unstored brick and relax renormalizes over the taps that exist, so
    // the sample comes out smoothed against a smaller neighbourhood. That is a
    // seam at a brick boundary, and invisible except as a measurement.
    //
    // sqrt(3) is the diagonal; 1.75 is it rounded up, because a brick of margin
    // costs a brick of fill and being short costs correctness. Margin on the
    // argument above rather than on a number: the whole-layer comparison this
    // was chased with turned out to be dominated by force-stored bricks past
    // the band, so widening the halo did not move it and the reasoning is what
    // stands behind this.
    const float reach = settings.region_radius + falloff +
                        static_cast<float>(radius) * cell +
                        1.75f * static_cast<float>(field::kBrickDim) * cell;
    return field::FieldVolume::Region::ball(settings.centre, reach);
}

SdfSculptDirty SdfSmoothTransaction::update(const field::RelaxSettings& settings,
                                            parallel::CancelToken* token) {
    SdfSculptDirty dirty;
    if (!live() || !field_source_) return dirty;

    // Bring in what this dab will READ, and only that. Bricks already
    // materialized are counted and left alone: refilling one would throw away
    // the edits earlier dabs made to it, which is the difference between a
    // cache and a bug.
    const field::FieldVolume::ResampleTally brought =
        working_.materialize_region(dependency_region(settings), field_source_->block_fill());
    materialized_.materialized_bricks += brought.added;
    materialized_.reused_bricks += brought.kept;
    ++materialized_.updates;
    // Cancelled while filling: what was materialized is source values and is
    // sound, but the dab has not run and must not run on a half-filled region.
    if (parallel::cancelled(token)) return dirty;

    // The dab itself, unchanged — the same in-place relax over the same
    // stencil. `rewrite_region` writes only bricks that store samples, and
    // everything this dab reads now does.
    const field::RelaxResult r = field::relax_in_place(working_, settings, token);
    dirty.bounds = r.dirty_bounds;
    dirty.touched_bricks = r.touched_bricks;
    dirty.changed = r.changed;
    if (r.changed) {
        changed_ = true;
        edited_.expand(r.dirty_bounds);
    }
    return dirty;
}

bool SdfSmoothTransaction::commit(scene::UndoStack* undo) {
    if (!live()) return false;
    scene::Document& doc = *doc_;
    const scene::Layer* layer = editable_layer(doc, layer_);
    // Refused rather than forced. The preview was computed from a layer that no
    // longer exists, and installing it would delete whatever replaced it
    // without the artist having asked for that.
    if (!layer || layer_fingerprint(*layer) != fingerprint_) {
        cancel();
        return false;
    }

    // A GESTURE THAT CHANGED NOTHING CHANGES NOTHING. No volume, no undo entry,
    // no consolidation, and the layer keeps every parametric item it had — a
    // pointer-down and pointer-up with no dab between them must not be a way to
    // lose an artist's history.
    if (!changed_ || !field_source_) {
        budget_ = SdfSculptBudget{};
        doc_ = nullptr;
        return true;
    }

    // The whole layer, once, through the SAME source the dabs materialized
    // from and on the SAME lattice. Not `bake_layer`: this is where the prefix
    // cache pays, and re-walking the history the dabs already paid for would
    // be the cost this design exists to remove.
    field::FieldVolume final_volume = field::FieldVolume::sample_blocks(
        field_source_->block_fill(), region_, working_.cell_size(), working_.band());
    if (final_volume.brick_count() == 0) {
        cancel();
        return false;
    }

    if (!edited_.empty()) {
        const field::FieldVolume::Region edited{edited_};
        // The edited bricks have to STORE samples before they can receive them:
        // a dab can move the surface into a brick the source classified as
        // empty, and `rewrite_region` writes only bricks that store something.
        final_volume.materialize_region(edited, field_source_->block_fill());
        // ...and then the dabs' own samples, over exactly the region they
        // changed. Identity outside it by construction — where the working
        // field has no sample the source's value stands — which is what
        // rewrite_region requires.
        final_volume.rewrite_region(edited, [this](int gx, int gy, int gz, float old) {
            const std::optional<float> mine = working_.sample_at(gx, gy, gz);
            return mine ? *mine : old;
        });
    }

    // The bake's post-process, once, at the end. This is the one place the lazy
    // path differs from the whole-layer one it replaces: that relaxed a
    // redistanced bake, and this redistances a relaxed field. Both are sound,
    // neither approximates the other, and the difference is measured rather
    // than asserted away.
    if (!field::redistance(final_volume)) {
        // Redistancing declined, so compacting would be dropping bricks on the
        // strength of samples nothing has earned the right to conclude from.
    } else {
        final_volume.compact();
    }
    final_volume.set_sample_lipschitz(final_volume.measure_sample_lipschitz());

    // ONE user-facing step, opened here and closed once, with the install's own
    // bracket and any consolidation's bracket nesting inside it — see
    // UndoStack::begin_group.
    if (undo) undo->begin_group();
    scene::ConsolidationCost installed_cost;
    const bool installed = scene::replace_layer_with_volume(doc, layer_, std::move(final_volume),
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
    // working field and its source are released because a cancelled preview is
    // not a thing a host should still be able to draw.
    working_ = field::FieldVolume();
    field_source_.reset();
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
