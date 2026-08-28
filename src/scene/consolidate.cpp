// The consolidation policy (scene-model spec, add-consolidation-policy). See
// include/clay/scene/consolidate.h for the decisions this implements: why the
// trigger is advisory, why the scope is a layer, and why a consolidated region
// is recognised by its content rather than by a stored provenance flag.

#include "clay/scene/consolidate.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "clay/field/redistance.h"
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
        out.longest_deformer_chain =
            std::max(out.longest_deformer_chain, static_cast<int>(n->deformers.size()));
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
    out.advises_consolidation =
        advise_below_step_scale > 0.0f && out.safe_step_scale < advise_below_step_scale;
    return out;
}

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
    const float band = params.band > 0.0f ? params.band : params.cell_size * 3.0f;
    const float padding = params.padding > 0.0f ? params.padding : band;

    const Layer view = local_view(layer);
    const Tape tape = compile_layer(view);
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
    if (layer_colors_vary(layer)) {
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

bool consolidate_layer(Document& doc, LayerId layer_id, const ConsolidationParams& params,
                       UndoStack* undo, ConsolidationCost* out_cost,
                       const BakePointEval& point_eval, parallel::CancelToken* token,
                       bool* out_cancelled) {
    if (out_cancelled) *out_cancelled = false;
    const Layer* layer = doc.find_layer(layer_id);
    if (!layer || layer->kind != LayerKind::Sdf || !layer->sdf) return false;
    // Checked before the bake, not after: a locked layer should not cost a
    // full resampling to say no. apply() refuses it again on the way through,
    // which is what keeps protection a property of the command rather than of
    // this entry point.
    if (layer->protected_from_edits()) return false;

    // Hidden roots are left alone. They contribute nothing to the field, so
    // absorbing them would discard their parameters in exchange for nothing —
    // and the artist hid them precisely to come back to them.
    std::vector<NodeId> absorb;
    for (NodeId id : layer->sdf->roots) {
        const Node* n = layer->sdf->find(id);
        if (n && n->visible) absorb.push_back(id);
    }
    if (absorb.empty()) return false;

    std::optional<field::FieldVolume> volume =
        bake_layer(*layer, params, out_cost, point_eval, token);
    // A cancel and "there was nothing to consolidate" both come back as
    // nullopt, and a host must not show the second when the user did the first.
    if (!volume && parallel::cancelled(token)) {
        if (out_cancelled) *out_cancelled = true;
        return false;  // the document is untouched: the bake had not been installed
    }
    if (!volume) return false;

    auto run = [&doc, undo](const Command& cmd) {
        if (undo) return undo->perform(doc, cmd);
        return scene::apply(doc, cmd).has_value();
    };

    // The sever and the bake go into ONE group, so a single undo puts back both
    // the items that were absorbed and the sharing that was severed.
    if (undo) undo->begin_group();
    const bool added = install_bake(doc, layer_id, absorb, std::move(*volume), run);
    if (undo) undo->end_group();
    return added;
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
