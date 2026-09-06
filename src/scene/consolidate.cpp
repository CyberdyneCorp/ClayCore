// The consolidation policy (scene-model spec, add-consolidation-policy). See
// include/clay/scene/consolidate.h for the decisions this implements: why the
// trigger is advisory, why the scope is a layer, and why a consolidated region
// is recognised by its content rather than by a stored provenance flag.

#include "clay/scene/consolidate.h"

#include "clay/scene/bounds.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "clay/field/redistance.h"
#include "clay/scene/bounds.h"
#include "clay/kernel/exactness.h"
#include "clay/scene/tape.h"

namespace clay {
namespace scene {

namespace {

void fill_cost(const field::FieldVolume& v, ConsolidationCost& cost) {
    cost.cell_size = v.cell_size();
    cost.band = v.band();
    cost.brick_count = v.brick_count();
    cost.sample_count = v.sample_count();
    cost.bytes = v.blob_floats() * sizeof(float);
    cost.sample_lipschitz = v.sample_lipschitz();
    // What the tape compiler will declare once this volume is an item: the
    // caller is being told the marching cost it is buying, not a raw slope.
    const kernel::CFieldInfo info = kernel::cfi_volume(v.sample_lipschitz());
    cost.lipschitz = info.lipschitz;
    cost.safe_step_scale = kernel::csafe_step_scale(info);
    cost.bounds = v.bounds();
}

// compile_layer treats a hidden layer as empty, which is right for drawing and
// wrong for both questions here: a hidden layer is still a layer whose chain
// has degraded, and reporting it as clean — or refusing to bake it — would make
// hiding a layer a way to get stuck in a state nothing will tell you about.
Layer visible_view(const Layer& layer) {
    Layer view = layer;
    view.visible = true;
    return view;
}

// ...and for a BAKE, the layer in its OWN frame as well. Sampling the
// world-space field and putting the result back under the layer would apply the
// layer's transform twice; sampling locally leaves the transform where it was
// authored, so consolidating moves nothing.
Layer local_view(const Layer& layer) {
    Layer view = visible_view(layer);
    view.xform = math::Transform{};
    return view;
}

// A node carrying samples and nothing else to edit.
bool is_volume_item(const Node& n) {
    return !n.is_group && n.prim.type == PrimType::Volume && n.volume != nullptr;
}

// Whether the layer can produce more than one colour, and so whether filling a
// colour channel buys anything. Filling it is a SECOND evaluation of the tape
// at every surviving sample; where the layer has one colour the result is that
// colour repeated, which the node's own colour already reports (scene-model:
// "the node's colour SHALL remain the answer ... for a volume with no colour").
//
// Two ways a layer holds more than one colour, and the second is why a test on
// node colours alone is not enough: a volume whose SAMPLES carry colour has one
// node colour and many sample colours, so a re-consolidated character would be
// silently flattened by the cheaper test.
//
// Every node is scanned — groups included, and hidden nodes included. Whether
// either reaches the tape depends on what the compiler does with it, and the
// asymmetry decides the doubt: being wrong in the direction of DOING the pass
// costs time on a document that already has more than one colour, and being
// wrong the other way loses colour. So a hidden red dab in a grey layer buys
// the pass it does not need, which is a slow bake rather than a wrong one.
bool color_varies(const SdfContent& content, const std::vector<NodeId>& ids) {
    std::vector<NodeId> pending(ids.rbegin(), ids.rend());
    bool seen = false;
    kernel::cfloat3 first = kernel::cf3(0, 0, 0);
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        const Node* n = content.find(id);
        if (!n) continue;
        if (is_volume_item(*n) && n->volume->has_color()) return true;
        if (!seen) {
            first = n->color;
            seen = true;
        } else if (n->color.x != first.x || n->color.y != first.y || n->color.z != first.z) {
            return true;
        }
        pending.insert(pending.end(), n->children.rbegin(), n->children.rend());
    }
    return false;
}

// Descend, counting nodes and the two things that degrade a chain. Written as
// an explicit stack rather than a recursion because the only reason to recurse
// here would be to walk a tree the arena can already walk flat.
void walk(const SdfContent& content, const std::vector<NodeId>& ids, FieldReport& out) {
    std::vector<NodeId> pending(ids.rbegin(), ids.rend());
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        const Node* n = content.find(id);
        if (!n) continue;
        ++out.item_count;
        if (!n->is_group) ++out.drawable_count;
        out.longest_deformer_chain =
            std::max(out.longest_deformer_chain, static_cast<int>(n->deformers.size()));
        out.steepest_deformer_chain =
            std::max(out.steepest_deformer_chain, deformer_lipschitz(*n));
        if (n->volume)
            out.steepest_volume = std::max(out.steepest_volume, n->volume->sample_lipschitz());
        pending.insert(pending.end(), n->children.rbegin(), n->children.rend());
    }
}

// -- the bake's grid path -----------------------------------------------------
//
// The serial bake evaluated the layer tape at every lattice sample through a
// std::function, one point at a time on one core. This is the same
// evaluation as a flat batch through the CPU backend: same tape, same points,
// and the backend's batch path is documented to match its scalar reference
// bit for bit, so the volume comes out byte-identical while every core
// participates. Blocks land in slot order whatever order the pool computed
// them in (sample_blocks assembles serially), so the bytes do not depend on
// thread scheduling either.
//
// The pool arrives as an INJECTED BakePointEval (eval/bake_points.h, passed
// down by the bindings and the benchmark) rather than by naming the backend
// here: the layering runs eval -> scene, never scene -> eval, and the
// injection is what lets this module stay below the registry it benefits
// from. With no evaluator — or one that declines — every window falls back
// to the serial walk below, same positions, same arithmetic, same bytes.
//
// WHY NOT PER-BRICK CULLED TAPES, when the refill path over the same bricks
// lives on them: the cull contract (scene/tape.h) is BAND-CLAMPED identity,
// and the bake is the one consumer that stores raw values past the band. It
// is not just the far samples that go soft, either — a chain of smooth
// blends can carry a culled item's quadratic tail from an accumulator value
// beyond the cull horizon down INTO the band (measured up to ~7e-3 on a
// 24-dab blend chain, dilating by the band exactly as the refill does). The
// refill never sees this because it clamps every stored value to ±band and
// its scenes pay it below its half-float precision; a bake that consolidated
// a layer to slightly different bytes than the serial bake would fail the
// determinism this feature is specified against. Culling here would need a
// per-op tail-freeness analysis to be sound — the win it offered is already
// covered by the pool, so the bake spends cores, not exactness.

// One window of bricks, every sample against the full tape, distances only.
// The loop below is the same evaluation again when no evaluator was injected.
void fill_window(const Tape& tape, const BakePointEval& point_eval,
                 const field::FieldVolume::BrickGrid& grid, std::size_t first,
                 std::size_t count, float* out) {
    const std::size_t n = count * field::kBrickSamples;
    std::vector<float> points(n * 3);
    for (std::size_t s = 0; s < count; ++s)
        for (int i = 0; i < field::kBrickSamples; ++i) {
            const kernel::cfloat3 p = grid.sample_position(first + s, i);
            const std::size_t at = (s * field::kBrickSamples + static_cast<std::size_t>(i)) * 3;
            points[at] = p.x;
            points[at + 1] = p.y;
            points[at + 2] = p.z;
        }
    if (point_eval && point_eval(tape, points.data(), n, out, nullptr)) return;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = tape.eval(kernel::cf3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2])).d;
}

}  // namespace

bool layer_colors_vary(const Layer& layer) {
    return layer.sdf && color_varies(*layer.sdf, layer.sdf->roots);
}

FieldReport report_layer(const Layer& layer, float advise_below_step_scale) {
    FieldReport out;
    const Tape tape = compile_layer(visible_view(layer));
    out.lipschitz = tape.info.lipschitz;
    out.safe_step_scale = tape.safe_step_scale();
    if (layer.sdf) walk(*layer.sdf, layer.sdf->roots, out);
    // WHAT THE ADVICE IS KEYED ON, and why it is no longer the step scale alone
    // (issue #387). A grab chain lowers `safe_step_scale` exactly as a stack of
    // baked volumes does, so an advisory that read only the aggregate fired
    // hardest on the case consolidation cannot help — measured 6x WORSE on a
    // real gesture, a 29x better step scale swamped by what it costs to
    // evaluate a warped 3.3 MB volume where the layer had held one analytic
    // primitive.
    //
    // Consolidation wins back two things and only two: the cost of walking an
    // EDIT LIST, and the Lipschitz of STACKED VOLUMES, which the bake
    // redistances away. A layer that is one drawable item carrying a brush
    // chain has neither, so there the bake is a straight loss. The report
    // already knew enough to say so; it just was not asked.
    const bool degraded =
        advise_below_step_scale > 0.0f && out.safe_step_scale < advise_below_step_scale;
    const bool volumes = out.steepest_volume > 1.0f || out.drawable_count > 1;
    const bool deformers = out.steepest_deformer_chain > 1.0f;
    if (degraded) {
        out.degradation = volumes ? (deformers ? Degradation::Both : Degradation::Volumes)
                                  : (deformers ? Degradation::Deformers : Degradation::Volumes);
    }
    // Advised whenever there IS something to absorb. On a layer degraded by
    // both, the bake fixes both — it is the deformer-only shape that is the
    // loss.
    out.advises_consolidation = degraded && volumes;
    return out;
}

namespace {
// Defined below, beside the phases it runs. Declared here because `bake_layer`
// is the door that opens the progress scope and then hands it over.
std::optional<field::FieldVolume> bake_tape_with(const Tape& tape,
                                                 const ConsolidationParams& params,
                                                 bool want_color, ConsolidationCost* out_cost,
                                                 const BakePointEval& point_eval,
                                                 parallel::CancelToken* token,
                                                 parallel::ProgressScope& progress);
}  // namespace

std::optional<field::FieldVolume> bake_layer(const Layer& layer,
                                             const ConsolidationParams& params,
                                             ConsolidationCost* out_cost,
                                             const BakePointEval& point_eval,
                                             parallel::CancelToken* token) {
    // Six phases, and a host drawing a bar needs to know which: sample,
    // redistance, compact, colour, measure, done. A single fraction would be a
    // lie, because their per-unit costs differ by more than an order.
    parallel::ProgressScope progress(token, 5);
    if (!(params.cell_size > 0.0f)) return std::nullopt;

    // The layer's own frame, and visible whatever the artist hid: sampling the
    // world-space field and putting the result back under the layer would apply
    // the transform twice, and a hidden layer is still a layer whose chain has
    // degraded. Both overrides are silent if missed, and both have a test.
    const Layer view = local_view(layer);
    const Tape tape = compile_layer(view);

    // ...and the one thing the TAPE cannot answer. `layer_colors_vary` walks
    // the node tree — groups and hidden nodes included — and the compiler has
    // already folded colour into instructions by the time a tape exists, so
    // there is no way back to the question from here. It is public for exactly
    // this reason: a caller baking a tape owes the same rule, or it gets
    // different bytes.
    return bake_tape_with(tape, params, layer_colors_vary(layer), out_cost, point_eval, token,
                          progress);
}

namespace {

// The half of a bake that only needs a TAPE, with the caller's progress scope
// so that a bake through either door reports the same five phases from the same
// moment. Split out rather than duplicated because the prefix cache samples a
// tape that belongs to no layer, and a second implementation of "sample,
// redistance, compact, colour, measure" would be a second definition of what a
// consolidated volume IS.
std::optional<field::FieldVolume> bake_tape_with(const Tape& tape,
                                                 const ConsolidationParams& params,
                                                 bool want_color, ConsolidationCost* out_cost,
                                                 const BakePointEval& point_eval,
                                                 parallel::CancelToken* token,
                                                 parallel::ProgressScope& progress) {
    const float band = params.band > 0.0f ? params.band : params.cell_size * 3.0f;
    const float padding = params.padding > 0.0f ? params.padding : band;

    if (tape.empty() || tape.bounds.empty() || tape.bounds.is_infinite()) return std::nullopt;

    math::Aabb region = params.region;
    if (region.empty()) {
        // Padded: sampling exactly to the bounds would clip the band at the
        // surface, which is where it is needed most.
        const kernel::cfloat3 pad = kernel::cf3(padding, padding, padding);
        region = math::Aabb{tape.bounds.min - pad, tape.bounds.max + pad};
    }

    progress.phase(0);
    bool cancelled = false;
    field::FieldVolume volume = field::FieldVolume::sample_blocks(
        [&tape, &point_eval](const field::FieldVolume::BrickGrid& grid, std::size_t first,
                             std::size_t count,
                             float* out) { fill_window(tape, point_eval, grid, first, count, out); },
        region, params.cell_size, band, token, &cancelled);
    if (cancelled) return std::nullopt;
    // brick_count rather than empty(): a volume covering only empty space
    // still has a full brick index, it just stores no samples, and handing one
    // back from a bake would replace the layer with something that silently
    // contributes nothing.
    if (volume.brick_count() == 0) return std::nullopt;

    // compact() only AFTER a successful redistance, never on its own: it
    // drops bricks on the strength of their samples all being past the band,
    // which a steep field does not entitle anyone to conclude. Redistancing is
    // what earns it, and it is what stops a repeatedly consolidated chain from
    // growing a brick of stored shell per bake.
    progress.phase(1);
    if (parallel::cancelled(token)) return std::nullopt;
    if (!params.skip_redistance && field::redistance(volume)) volume.compact();
    progress.phase(2);
    if (parallel::cancelled(token)) return std::nullopt;

    // The colours the bake used to discard. Consolidation is advertised as
    // changing what a layer COSTS rather than what it looks like, and
    // collapsing every colour in it to the one on the resulting node
    // contradicted that: a consolidated character lost the distinction between
    // skin and armour.
    //
    // AFTER redistance and compact, so colour is filled for the samples that
    // actually survive rather than for bricks compact is about to drop. It is
    // a second pass over those samples — the batched fill above returns
    // distances only — and it is charged to consolidation, which is an
    // operation with progress UI rather than a frame.
    // Through the SAME injected evaluator the distances went through. A serial
    // colour pass beside a pooled distance pass makes the pooled bake no
    // faster than the serial one it replaced — measured, by the benchmark gate
    // that compares exactly those two.
    //
    // And SKIPPED ENTIRELY where the layer holds one colour, which is what
    // pooling it was not enough to fix: pooled or not, it is a second
    // evaluation of the tape at every surviving sample, and a grey sculpt was
    // paying it to recover a constant. Measured on the reference iPad at 916 ms
    // against a 786 ms budget, where the release before colour landed took
    // 524 ms.
    progress.phase(3);
    if (want_color) {
        volume.fill_colors_blocks([&tape, &point_eval](const float* points_xyz, std::size_t count,
                                                      float* out_rgb) {
            std::vector<float> scratch(count);
            if (point_eval && point_eval(tape, points_xyz, count, scratch.data(), out_rgb)) return;
            for (std::size_t i = 0; i < count; ++i) {
                const kernel::CTapeValue v = tape.eval(
                    kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]));
                out_rgb[i * 3] = v.color.x;
                out_rgb[i * 3 + 1] = v.color.y;
                out_rgb[i * 3 + 2] = v.color.z;
            }
        });
    }
    // Re-measured because redistance and compact both changed the samples
    // since sample() measured them. Declaring anything smaller than this would
    // be the overstep the bound exists to prevent, so it is measured rather
    // than reasoned about at every step that could have moved it.
    progress.phase(4);
    // The last checkpoint before the caller commits. Past here the operation is
    // cheap and a cancel would only delay the same result.
    if (parallel::cancelled(token)) return std::nullopt;
    volume.set_sample_lipschitz(volume.measure_sample_lipschitz());

    if (out_cost) fill_cost(volume, *out_cost);
    return volume;
}

}  // namespace

std::optional<field::FieldVolume> bake_tape(const Tape& tape, const ConsolidationParams& params,
                                            bool want_color, ConsolidationCost* out_cost,
                                            const BakePointEval& point_eval,
                                            parallel::CancelToken* token) {
    // The same five phases, opened here because this is a whole operation when
    // it is entered directly. `bake_layer` opens its own before compiling, so
    // a host polling progress sees the compile as phase 0 exactly as it always
    // has.
    parallel::ProgressScope progress(token, 5);
    if (!(params.cell_size > 0.0f)) return std::nullopt;
    return bake_tape_with(tape, params, want_color, out_cost, point_eval, token, progress);
}

// COPY-ON-WRITE before a bake, because a bake is about ONE subtool.
//
// An instance layer shares its `SdfContent` by shared_ptr, and consolidation
// edits that content through the command vocabulary. Baking in place would
// therefore replace the edit list of every layer instancing it — nine subtools
// silently collapsing into a volume because the artist baked the tenth, which
// is not a reading of "this shape is finished" anyone asks for.
//
// Expressed as the remove-then-add pair clay_document_move_layer already uses,
// rather than by assigning `layer->sdf` directly, and that is the point: both
// halves are ordinary commands, so the sever serializes, journals and undoes
// like everything else. RemoveLayerCmd's inverse carries the Layer BY VALUE
// with its original shared_ptr intact, so one undo of the whole consolidation
// puts the layer back sharing the content it shared before.
//
// Returns false only when the pair could not be applied; a layer that shares
// with nobody is left exactly alone.
bool sever_shared_content(Document& doc, LayerId layer_id,
                          const std::function<bool(const Command&)>& run) {
    const Layer* layer = doc.find_layer(layer_id);
    if (!layer || !layer->sdf) return true;
    // Counted over the document's LAYERS rather than by use_count(): an undo
    // stack holds inverses carrying a Layer by value, so a layer that was
    // removed and put back has a use count above one while sharing with
    // nobody. Severing it would cost a deep copy on a document that never
    // instanced anything.
    std::size_t sharers = 0;
    for (const Layer& l : doc.layers)
        if (l.sdf == layer->sdf) ++sharers;
    if (sharers <= 1) return true;

    Layer severed = *layer;
    severed.sdf = std::make_shared<SdfContent>(*layer->sdf);
    int index = -1;
    for (std::size_t i = 0; i < doc.layers.size(); ++i)
        if (doc.layers[i].id == layer_id) index = static_cast<int>(i);

    if (!run(Command{RemoveLayerCmd{layer_id}})) return false;
    return run(Command{AddLayerCmd{std::move(severed), index}});
}

// Everything the bake CHANGES, as commands, from the sever to the installed
// volume. Split out of consolidate_layer so the group bracket around it is
// opened and closed in exactly one place: with the sever's two exits inline,
// `end_group` had to be repeated at each of them, which is the shape a missed
// one hides in.
bool install_bake(Document& doc, LayerId layer_id, const std::vector<NodeId>& absorb,
                  field::FieldVolume volume, const std::function<bool(const Command&)>& run) {
    // First, so that nothing below can reach a layer this one only borrows.
    if (!sever_shared_content(doc, layer_id, run)) return false;
    // The sever removed and reinserted the layer, so any pointer taken before
    // it is dangling — and the content behind it may now be a different object.
    const Layer* layer = doc.find_layer(layer_id);
    if (!layer || !layer->sdf) return false;

    Node baked;
    // Reserved from the layer's OWN content: reserving from the shared object
    // would advance an id counter that belongs to every other instance.
    baked.id = layer->sdf->reserve_id();
    baked.prim = Prim::volume();
    // The bake sampled the layer's field, mirror copies included, so the
    // volume already holds both sides. Re-mirroring it is idempotent for the
    // union but doubles what every later evaluation pays.
    baked.mirror = false;
    baked.volume = std::make_shared<const field::FieldVolume>(std::move(volume));
    // One colour for what may have been many. A volume carries a single
    // colour, so the first absorbed item's is the one that survives — stated
    // here because it is a loss, not a detail.
    if (const Node* first = layer->sdf->find(absorb.front())) baked.color = first->color;

    int index = -1;
    NodeId parent = kNoNode;
    layer->sdf->locate(absorb.front(), &parent, &index);

    // Removed last-first so that the recorded inverses, replayed in reverse on
    // undo, reinsert at ascending indices — which is what puts the edit list
    // back in its original order rather than reversed.
    for (auto it = absorb.rbegin(); it != absorb.rend(); ++it)
        run(Command{RemoveNodeCmd{layer_id, *it}});
    return run(Command{AddNodeCmd{layer_id, parent, index, std::vector<Node>{std::move(baked)}}});
}

namespace {

// The roots a bake would absorb, or nothing when there is no layer to bake.
//
// Hidden roots are left alone. They contribute nothing to the field, so
// absorbing them would discard their parameters in exchange for nothing — and
// the artist hid them precisely to come back to them.
//
// The protection check lives here so it happens BEFORE the bake, not after: a
// locked layer should not cost a full resampling to say no. apply() refuses it
// again on the way through, which is what keeps protection a property of the
// command rather than of an entry point.
bool absorbable_roots(const Document& doc, LayerId layer_id, std::vector<NodeId>* out) {
    const Layer* layer = doc.find_layer(layer_id);
    if (!layer || layer->kind != LayerKind::Sdf || !layer->sdf) return false;
    if (layer->protected_from_edits()) return false;
    for (NodeId id : layer->sdf->roots) {
        const Node* n = layer->sdf->find(id);
        if (n && n->visible) out->push_back(id);
    }
    return !out->empty();
}

}  // namespace

bool replace_layer_with_volume(Document& doc, LayerId layer_id, field::FieldVolume volume,
                               UndoStack* undo, ConsolidationCost* out_cost) {
    std::vector<NodeId> absorb;
    if (!absorbable_roots(doc, layer_id, &absorb)) return false;
    // Reported from the volume that is about to be installed, so a caller that
    // computed its own gets the same numbers a bake would have handed it.
    if (out_cost) fill_cost(volume, *out_cost);

    auto run = [&doc, undo](const Command& cmd) {
        if (undo) return undo->perform(doc, cmd);
        return scene::apply(doc, cmd).has_value();
    };

    // The sever and the bake go into ONE group, so a single undo puts back both
    // the items that were absorbed and the sharing that was severed. Nested
    // inside a caller's own group this collapses into it rather than opening a
    // second step — see UndoStack::begin_group — which is what lets a sculpt
    // transaction commit its stroke and consolidate it as one undo.
    if (undo) undo->begin_group();
    const bool added = install_bake(doc, layer_id, absorb, std::move(volume), run);
    if (undo) undo->end_group();
    return added;
}

RegionMerge plan_region_merge(const Layer& layer, const math::Aabb& region) {
    RegionMerge out;
    if (layer.kind != LayerKind::Sdf || !layer.sdf) return out;
    if (region.empty()) return out;

    // The candidates and their reach, computed once. An influence bound is not
    // free — a non-local op reports an infinite one, a mirrored item spans the
    // plane — and the fixed point below would otherwise ask for each of them
    // once per round.
    std::vector<NodeId> ids;
    std::vector<math::Aabb> reach;
    for (NodeId id : layer.sdf->roots) {
        const Node* n = layer.sdf->find(id);
        if (!n || !n->visible) continue;  // hidden roots are left alone, as ever
        ids.push_back(id);
        reach.push_back(node_influence_bound(*layer.sdf, id, layer));
    }
    if (ids.empty()) return out;

    // The closure. Each round takes everything the box now meets; the box then
    // grows to cover their reach. It terminates because `taken` only ever gains
    // members and there are finitely many, so the loop runs at most once per
    // item — and in practice twice, since the second round usually adds
    // nothing.
    math::Aabb box = region;
    std::vector<bool> taken(ids.size(), false);
    bool grew = true;
    while (grew) {
        grew = false;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (taken[i]) continue;
            // An INFINITE reach meets every box and is contained by none, so an
            // item carrying one pulls the closure out to the whole layer. That
            // is the correct answer rather than a special case: an item that
            // can change the field anywhere can change it inside the box, so
            // leaving it behind would break the property this exists for.
            if (reach[i].empty()) continue;  // reaches nothing, so it meets nothing
            if (!reach[i].is_infinite() && !box.intersects(reach[i])) continue;
            taken[i] = true;
            grew = true;
            if (reach[i].is_infinite()) {
                // "Everywhere" has no box, so it resolves to the union of every
                // FINITE reach in the layer — which is where the field can
                // actually differ, and is what a whole-layer bake samples.
                for (const math::Aabb& r : reach)
                    if (!r.empty() && !r.is_infinite()) box.expand(r);
            } else {
                box.expand(reach[i]);
            }
        }
    }

    for (std::size_t i = 0; i < ids.size(); ++i)
        if (taken[i]) out.absorb.push_back(ids[i]);
    if (out.absorb.empty()) return out;  // the region reached nothing
    out.box = box;
    out.whole_layer = out.absorb.size() == ids.size();
    return out;
}

bool consolidate_region(Document& doc, LayerId layer_id, const math::Aabb& region,
                        const ConsolidationParams& params, UndoStack* undo,
                        ConsolidationCost* out_cost, const BakePointEval& point_eval,
                        parallel::CancelToken* token, bool* out_cancelled,
                        RegionMerge* out_plan) {
    if (out_cancelled) *out_cancelled = false;
    // The same gate `absorbable_roots` applies, and for the same reason: a
    // protected layer should not cost a resampling to say no.
    std::vector<NodeId> all;
    if (!absorbable_roots(doc, layer_id, &all)) return false;
    const Layer* layer = doc.find_layer(layer_id);

    const RegionMerge plan = plan_region_merge(*layer, region);
    if (out_plan) *out_plan = plan;
    if (plan.absorb.empty()) return false;

    // The closure, not the caller's region. Sampling the region alone would
    // leave the absorbed items' contribution outside it in nothing at all.
    ConsolidationParams over_closure = params;
    over_closure.region = plan.box;
    std::optional<field::FieldVolume> volume =
        bake_layer(*layer, over_closure, out_cost, point_eval, token);
    if (!volume && parallel::cancelled(token)) {
        if (out_cancelled) *out_cancelled = true;
        return false;  // untouched: the bake had not been installed
    }
    if (!volume) return false;

    auto run = [&doc, undo](const Command& cmd) {
        if (undo) return undo->perform(doc, cmd);
        return scene::apply(doc, cmd).has_value();
    };
    // One group, exactly as the whole-layer form takes: a single undo puts back
    // every absorbed item and the sharing the sever broke.
    if (undo) undo->begin_group();
    const bool added = install_bake(doc, layer_id, plan.absorb, std::move(*volume), run);
    if (undo) undo->end_group();
    return added;
}

bool consolidate_layer(Document& doc, LayerId layer_id, const ConsolidationParams& params,
                       UndoStack* undo, ConsolidationCost* out_cost,
                       const BakePointEval& point_eval, parallel::CancelToken* token,
                       bool* out_cancelled) {
    if (out_cancelled) *out_cancelled = false;
    std::vector<NodeId> absorb;
    if (!absorbable_roots(doc, layer_id, &absorb)) return false;
    const Layer* layer = doc.find_layer(layer_id);

    std::optional<field::FieldVolume> volume =
        bake_layer(*layer, params, out_cost, point_eval, token);
    // A cancel and "there was nothing to consolidate" both come back as
    // nullopt, and a host must not show the second when the user did the first.
    if (!volume && parallel::cancelled(token)) {
        if (out_cancelled) *out_cancelled = true;
        return false;  // the document is untouched: the bake had not been installed
    }
    if (!volume) return false;

    // `out_cost` is already filled by the bake, from the same volume, so the
    // installer is not asked for it a second time.
    return replace_layer_with_volume(doc, layer_id, std::move(*volume), undo, nullptr);
}

// -- the advice ---------------------------------------------------------------

namespace {

// Cells across the smallest feature the layer carries. kBrickDim is 8, so four
// puts that feature across half a brick. MEASURED rather than assumed: see
// design.md's "what building it found" for the sweep of 2, 4 and 8 against the
// parametric field and what it settled.
constexpr float kCellsPerFeature = 4.0f;
// The GRID is clamped, not the memory. A banded volume stores only surface
// bricks, so the byte count follows surface area and is not predictable from
// either bound — which is why the advice quotes `bytes` at the number it
// advises and expects a host to refuse on that rather than on the clamp. These
// only have to keep the sampling pass itself affordable.
constexpr float kFinestGrid = 512.0f;
constexpr float kCoarsestGrid = 32.0f;

// The best safe step scale any consolidated layer can declare. A sampled
// volume declares sqrt(3) times its samples' Lipschitz (cfi_volume) and a
// redistanced volume's samples measure ~1, so 1/sqrt(3) is the ceiling. A
// caller asking for more is asking for something no bake delivers, and it is
// cased here rather than left to fall out of the projection so that the answer
// costs nothing instead of a full sampling pass.
constexpr float kBestConsolidatedStepScale = 0.5773503f;

// What one drawable node contributes as a feature, in the frame `view` is
// expressed in. Zero when it contributes none.
float node_feature(const Layer& view, const Node& n) {
    // A volume states its own resolution, so hand it back: (K * c) / K = c is
    // the whole answer to "a number nobody chose". Carried into this frame,
    // because the node's and the layer's scales are part of the box the bake
    // samples — a volume under a half-scale node is half as fine here as it is
    // in its own lattice. `placed_distance_scale` is the conservative
    // (smallest-stretch) factor, so it errs towards a finer grid.
    if (is_volume_item(n) && n.volume->sample_count() > 0)
        return kCellsPerFeature * n.volume->cell_size() * placed_distance_scale(view, n);

    // Otherwise the SHAPE's own extent, carried by the same factor. NOT
    // `item_geometry_bound`, which the design named and which building this
    // refuted: that box is dilated by rounding and blend support, so a 0.06
    // dab carrying a quadratic blend measures 0.24 there rather than 0.12 and
    // the advice came out at HALF the cells it meant to put across it —
    // measured on the K sweep's own fixture, 0.060 where the shape says 0.030,
    // which moves the dab's surface by 27% of its radius rather than 7%.
    const math::Aabb b = item_local_bounds(n);
    if (b.empty() || b.is_infinite()) return 0.0f;
    const kernel::cfloat3 e = b.extent();
    return std::min({e.x, e.y, e.z}) * placed_distance_scale(view, n);
}

// The smallest thing the layer carries, in the frame `view` is expressed in.
// Zero when nothing finite was found.
float smallest_feature(const Layer& view) {
    float out = 0.0f;
    std::vector<NodeId> pending(view.sdf->roots.rbegin(), view.sdf->roots.rend());
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        const Node* n = view.sdf->find(id);
        if (!n || !n->visible) continue;  // hidden subtrees reach the field nowhere
        pending.insert(pending.end(), n->children.rbegin(), n->children.rend());
        if (n->is_group) continue;  // a transform and a name; nothing is evaluated
        const float f = node_feature(view, *n);
        if (f > 0.0f && (out == 0.0f || f < out)) out = f;
    }
    return out;
}

}  // namespace

ConsolidationParams advised_params(const Layer& layer) {
    ConsolidationParams out;  // cell_size == 0: nothing to derive from
    if (layer.kind != LayerKind::Sdf || !layer.sdf) return out;

    // The LOCAL frame, which is the one the bake samples. Deriving from
    // `layer_bounds` would compose the layer transform and advise a cell size
    // wrong by the layer's scale — and right on every layer at identity, so
    // the mistake would not show up until a host scaled a subtool.
    const Layer view = local_view(layer);
    const Tape tape = compile_layer(view);
    if (tape.empty() || tape.bounds.empty() || tape.bounds.is_infinite()) return out;
    const kernel::cfloat3 span = tape.bounds.extent();
    const float extent = std::max({span.x, span.y, span.z});
    if (!(extent > 0.0f)) return out;

    // No finite feature anywhere resolves to the coarsest grid rather than to
    // a refusal: the layer has an extent, so it has a bake, and the clamp is
    // the honest answer for a shape that declares no size of its own.
    float feature = smallest_feature(view);
    if (!(feature > 0.0f)) feature = extent;

    const float cell = std::clamp(feature / kCellsPerFeature, extent / kFinestGrid,
                                  extent / kCoarsestGrid);
    out.cell_size = cell;
    // Spelled out rather than left as the zeros that mean the same thing
    // today: a host that STORES the advice and re-bakes a week later must get
    // the same box, and a cost measured at 3 * cell beside params reading 0
    // describes two different bakes.
    out.band = 3.0f * cell;
    out.padding = out.band;
    // Not a default carried through — the point. Redistancing is what bounds
    // the Lipschitz, so advising a skip would advise a bake that does not cure
    // what the flag reported.
    out.skip_redistance = false;
    return out;
}

ConsolidationAdvice consolidation_advice(const Layer& layer, float advise_below_step_scale,
                                         const BakePointEval& point_eval) {
    ConsolidationAdvice out;
    if (!(advise_below_step_scale > 0.0f)) return out;
    if (advise_below_step_scale > kBestConsolidatedStepScale) return out;
    if (layer.kind != LayerKind::Sdf || !layer.sdf) return out;
    // Advising a bake that `consolidate_layer` will refuse is bad advice
    // rather than an error: a host walking a stack of mixed kinds gets "not
    // advised" for all of them and special-cases none.
    if (layer.protected_from_edits()) return out;
    if (!report_layer(layer, advise_below_step_scale).advises_consolidation) return out;

    const ConsolidationParams params = advised_params(layer);
    if (!(params.cell_size > 0.0f)) return out;

    ConsolidationCost cost;
    if (!bake_layer(layer, params, &cost, point_eval)) return out;
    // The verdict follows the NUMBER, not only the mechanism (#387 one step
    // on). A projection that does not reach the caller's threshold is the
    // honest answer for every shape that produces it — an already-consolidated
    // layer whose samples are still steep, contents that resample no better
    // than they evaluate, a redistance that cannot recover the shape — and it
    // is the same answer for all of them.
    if (cost.safe_step_scale < advise_below_step_scale) return out;

    out.advises = true;
    out.params = params;
    out.cost = cost;
    return out;
}

bool consolidation_state(const Layer& layer, ConsolidationCost* out_cost) {
    if (!layer.sdf || layer.sdf->roots.size() != 1) return false;
    const Node* n = layer.sdf->find(layer.sdf->roots.front());
    if (!n || !is_volume_item(*n)) return false;
    if (out_cost) fill_cost(*n->volume, *out_cost);
    return true;
}

}  // namespace scene
}  // namespace clay
