#pragma once

// The consolidation policy (scene-model spec, add-consolidation-policy):
// WHEN a chained edit has degraded far enough to be worth collapsing, WHAT it
// costs to collapse it, and HOW the baked result rejoins a document that is
// still parametric everywhere else.
//
// The verbs themselves are not the gap. `relax`, `flatten`, `snakehook`,
// `move_surface`, `moved_topologically_from` and the mask brush all landed and
// all work once. What they do not do is compose, and the reason is measurable:
//
//   * a polish samples a document and hands back a volume, so the SECOND pass
//     samples a volume rather than a document, and the declared Lipschitz goes
//     from 1.7 to 24 to 39 across three passes whatever the falloff;
//   * a Move stroke never touches a volume at all — each drag appends a `grab`
//     to the deformer chain, and where the drags OVERLAP deformer_lipschitz
//     multiplies them, so the safe step scale decays by a constant factor per
//     drag: 79x the marching cost by nine.
//
//     Only where they overlap, as of 0.69.0 (issue #386). A grab is the
//     identity outside its own ball, so drags that cannot reach one another no
//     longer compound and eight spread around a form cost what one costs. That
//     is the common shape of a Move session and it stopped being a reason to
//     consolidate; a stroke worked repeatedly over ONE area is the shape that
//     still is, and it is the shape this file was written for.
//
// TWO MECHANISMS, and a policy keyed on only one of them would miss the other.
// That is why the report below names the steepest volume and the longest
// deformer chain separately rather than only the aggregate: the aggregate says
// something is wrong, and those two say which thing.
//
// -- the decisions this file records ----------------------------------------
//
// THE TRIGGER IS ADVISORY, AND THE THRESHOLD IS THE CALLER'S. `report_layer`
// measures; it never bakes. A bake discards the parameters of everything it
// absorbs, and an engine that did that on its own would be deciding, on an
// artist's behalf and without being asked, that a sphere's radius is no longer
// editable. The threshold is an ARGUMENT rather than document state for the
// same reason it is not automatic: a tolerance for marching cost belongs to a
// viewport, a device and a frame budget, not to the artwork. Storing it in the
// document would put a rendering preference in the file and would need a
// format bump to carry it.
//
// THE SCOPE IS A LAYER. Not an arbitrary run of siblings: an edit list is
// ordered and its operators are relative, so a Subtract in the middle of a
// list means nothing without what precedes it, and "consolidate items 3..7"
// has no well-defined field of its own. A layer does: layers combine by hard
// union at the document level, so a layer's field is self-contained and baking
// one is exact with respect to the whole document's result. Both measured
// failures also live inside one layer — a polish chain and a drag stroke are
// each one layer's worth of edits.
//
// WHAT A HOST MAY STILL PROMISE, and how it can tell. `consolidation_state`
// answers from the CONTENT rather than from a stored provenance flag: a layer
// is consolidated when its edit list is a single item carrying samples. That
// is deliberate. The promise a host has to make is about what the region IS —
// samples at a fixed resolution, no parameters to offer — and a mesh imported
// with `volume_from_mesh` is exactly as unparametric as a bake, so a flag
// saying "this one came from a bake" would divide the cases a host must treat
// alike. It would also need a serialised field, and therefore a format bump,
// to survive a save.
//
// Re-expansion is ONE-WAY and this file does not offer it. What was absorbed
// is in the undo record, which is where a "go back" belongs; a separate
// un-bake would have to invent parameters for a shape that no longer has any.

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "clay/field/volume.h"
#include "clay/scene/commands.h"
#include "clay/parallel/cancel.h"
#include "clay/scene/document.h"

namespace clay {
namespace scene {

struct Tape;

// How bake_layer evaluates the layer's tape at its lattice samples. Empty —
// the default — walks tape.eval serially; eval/bake_points.h supplies
// pooled_bake_eval(), the same points through the CPU backend's reference
// arithmetic on its thread pool, which the bindings and the benchmark inject.
// An INJECTED evaluator is how the pool reaches a bake at all: eval depends
// on scene, never the reverse, so this module cannot name a backend. An
// evaluator may change only speed, never bytes — the volume is byte-identical
// either way (test_consolidate.cpp holds it to that), and one that cannot
// serve the request returns false to hand the window back to the serial walk.
// `out_colors_rgb` is null when the caller wants distances only, and count*3
// floats when it wants the colours too — the bake needs both, at the same
// points, and asking twice would evaluate the tape twice.
using BakePointEval = std::function<bool(const Tape& tape, const float* points_xyz,
                                         std::size_t count, float* out_distances,
                                         float* out_colors_rgb)>;

// What a layer's chain currently costs the marcher, and what is causing it.
struct FieldReport {
    float lipschitz = 1.0f;        // the compiled tape's declared bound
    float safe_step_scale = 1.0f;  // 1 / max(lipschitz, 1)
    // The two degradation mechanisms, separately, because they have different
    // cures and an aggregate cannot tell them apart.
    float steepest_volume = 1.0f;  // largest sample_lipschitz among volume items
    int longest_deformer_chain = 0;
    int item_count = 0;  // nodes in the layer's tree, groups included
    // safe_step_scale < the caller's threshold. Advice, never an action.
    bool advises_consolidation = false;
};

// Where a layer's field would be resampled, and how well.
struct ConsolidationParams {
    // Required: a document has no intrinsic scale to derive a resolution from
    // the way a mesh's own bounds give one, and guessing here would silently
    // fix the shape's resolution at a number nobody chose.
    float cell_size = 0.0f;
    float band = 0.0f;     // <= 0 means three cells
    float padding = 0.0f;  // past the layer's bounds; <= 0 means the band
    // Where to sample. Empty — the default — means the layer's own bounds
    // padded, which is what a caller wants once. A caller that consolidates
    // the SAME region repeatedly should pin it: a volume's geometric bound is
    // its whole sampled box, so each bake would otherwise pad the previous
    // padding and the box would creep outwards by two paddings a time.
    math::Aabb region;
    // Redistancing is what actually bounds the Lipschitz — see
    // field/redistance.h — so it is on unless a caller opts out, and the
    // opt-out is spelled as a SKIP so that a zeroed struct gets the sound
    // behaviour rather than the fast one.
    bool skip_redistance = false;
};

// What consolidating would spend, reported from what a volume already knows.
struct ConsolidationCost {
    float cell_size = 0.0f;
    float band = 0.0f;
    std::size_t brick_count = 0;
    std::size_t sample_count = 0;
    std::size_t bytes = 0;
    float sample_lipschitz = 1.0f;  // how fast the stored samples vary
    float lipschitz = 1.0f;         // what the compiler will declare for them
    float safe_step_scale = 1.0f;
    math::Aabb bounds;
};

// Measure a layer's chain. `advise_below_step_scale` is the caller's tolerance
// for marching cost; pass 0 to measure without asking for advice.
FieldReport report_layer(const Layer& layer, float advise_below_step_scale = 0.0f);

// Whether baking this layer will carry a colour channel, which it does exactly
// when the layer can produce more than one colour: two or more distinct node
// colours in its tree, or a node whose volume carries colour in its SAMPLES —
// the second because such a node has one colour and its samples have many, so a
// test on node colours alone would flatten a re-consolidated layer.
//
// Public because filling that channel is a SECOND evaluation of the tape at
// every surviving sample, so it is a real part of what consolidation costs and
// a caller may want to know before paying it. It is also the rule any reference
// implementation of the bake has to apply to get the same bytes.
bool layer_colors_vary(const Layer& layer);

// Sample a layer's field into a volume, without touching the document. This is
// the "what would it cost" half: the numbers it reports are the numbers the
// real thing produces, because it IS the real thing with the result thrown
// away. An estimate that skipped the sampling could not report a brick count,
// which is where the memory actually is.
//
// Baked in the layer's LOCAL frame, so the layer's own transform still applies
// to the result and consolidating does not move anything.
std::optional<field::FieldVolume> bake_layer(const Layer& layer,
                                             const ConsolidationParams& params,
                                             ConsolidationCost* out_cost = nullptr,
                                             const BakePointEval& point_eval = {},
                                             parallel::CancelToken* token = nullptr);

// Sample an already-compiled TAPE into a volume: the half of `bake_layer` that
// does not need a layer.
//
// Same sampling, same redistance-then-compact, same colour pass, same measured
// Lipschitz — it IS `bake_layer`'s body, so a volume built here is one a
// consolidation could have produced and there is one definition of what a
// baked volume is rather than two.
//
// The caller owes the two things a tape cannot state for itself:
//
//   * THE FRAME. `bake_layer` compiles a `local_view` — the layer visible and
//     its own transform identity — because sampling the world-space field and
//     putting the result back under the layer applies the transform twice. A
//     caller compiling its own tape owes the same convention.
//   * `want_color`. The compiler folds colour into instructions, so by the time
//     a tape exists the question "can this produce more than one colour" cannot
//     be asked of it. `layer_colors_vary` is public for exactly this reason,
//     and passing `true` unconditionally adds a channel a one-colour bake does
//     not have — which is different bytes, not a slower path.
//
// `tape` is BORROWED and must outlive the call.
std::optional<field::FieldVolume> bake_tape(const Tape& tape,
                                            const ConsolidationParams& params, bool want_color,
                                            ConsolidationCost* out_cost = nullptr,
                                            const BakePointEval& point_eval = {},
                                            parallel::CancelToken* token = nullptr);

// Collapse a layer's edit list into one volume item, as ONE undo step.
//
// The step is a group of removals plus one add, so its inverse is the group's
// inverse: the absorbed items come back with their ids, parameters, colours
// and deformers intact, because RemoveNodeCmd's inverse already carries the
// whole subtree by value. Nothing here invents a new command — the undo
// vocabulary could already express this, which is why the change adds a policy
// rather than a verb.
//
// Refused on a protected layer, and refused before the bake rather than after
// it, so a locked layer does not cost a full resampling to say no.
//
// `undo` may be null, in which case the commands are applied and no undo entry
// is recorded — the same choice every other editing entry point offers.
bool consolidate_layer(Document& doc, LayerId layer, const ConsolidationParams& params,
                       UndoStack* undo = nullptr, ConsolidationCost* out_cost = nullptr,
                       const BakePointEval& point_eval = {},
                       // Cancellable (add-operation-cancellation). Returns
                       // false with the document UNCHANGED when the token is
                       // set: the bake builds a volume and installs it at the
                       // end, so a cancel is a discard rather than a partial
                       // commit. `out_cancelled` tells that apart from "there
                       // was nothing to consolidate", which also returns false.
                       parallel::CancelToken* token = nullptr,
                       bool* out_cancelled = nullptr);

// Install an ALREADY-COMPUTED volume as the layer's single item, as one undo
// step — the second half of `consolidate_layer` without the first.
//
// Consolidation is two things that happen to be sold together: sample the
// layer's field into a volume, then replace the layer's edit list with it. A
// live Smooth stroke has already done the first half, once, at pointer-down,
// and has been mutating that volume locally ever since; making it bake the
// layer AGAIN at pointer-up to reach the installer would throw away the whole
// point of holding the volume. So the installer is a function.
//
// Everything the collapsed form guarantees is guaranteed here, because it is
// the same code: the sever of shared instance content, the removals and the
// add as ONE group whose inverse restores the absorbed items with their ids,
// parameters, colours and deformers, the refusal on a protected layer, the
// layer's own transform left where it was authored, and the first absorbed
// item's colour surviving onto the volume.
//
// What it does NOT do is check that `volume` is a plausible bake of this
// layer. It cannot: a volume is a volume. The caller owns that claim, which is
// exactly what a sculpt transaction is for.
bool replace_layer_with_volume(Document& doc, LayerId layer, field::FieldVolume volume,
                               UndoStack* undo = nullptr,
                               ConsolidationCost* out_cost = nullptr);

// What a host may still promise about a layer: whether its edit list is a
// single item carrying samples, and at what resolution. False for anything
// else, including a layer that merely contains a volume among other items —
// there the parameters of the other items are still there to offer.
bool consolidation_state(const Layer& layer, ConsolidationCost* out_cost = nullptr);

}  // namespace scene
}  // namespace clay
