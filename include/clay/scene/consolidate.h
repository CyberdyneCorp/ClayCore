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
//     to the deformer chain and deformer_lipschitz multiplies them, so the
//     safe step scale decays by a constant factor per drag, 79x the marching
//     cost by nine.
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

#include <optional>
#include <vector>

#include "clay/field/volume.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"

namespace clay {
namespace scene {

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
                                             ConsolidationCost* out_cost = nullptr);

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
                       UndoStack* undo = nullptr, ConsolidationCost* out_cost = nullptr);

// What a host may still promise about a layer: whether its edit list is a
// single item carrying samples, and at what resolution. False for anything
// else, including a layer that merely contains a volume among other items —
// there the parameters of the other items are still there to offer.
bool consolidation_state(const Layer& layer, ConsolidationCost* out_cost = nullptr);

}  // namespace scene
}  // namespace clay
