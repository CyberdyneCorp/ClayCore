#pragma once

// WHAT DOES THIS DOCUMENT COST?
//
// iOS asks this question, not the user, and it does not ask politely:
// `didReceiveMemoryWarning` arrives with no argument and expects an answer
// within a frame or two. To decide what to release a host must know what it is
// holding, and until `roll-up-document-memory` this library could not tell it.
//
// Every subsystem accounted for itself and NOTHING ROLLED UP. The history
// reported its bytes, one grid reported its sculpt layers, the brick cache
// reported a cache the document does not own — and the edit list, the voxel
// chunk storage those sculpt layers sit beside, masks, mesh layers and the
// passthrough blobs reported nothing at all. The rest is where the memory is: a
// rasterized 256^3 voxel layer is the largest thing most documents hold and it
// was invisible.
//
// WHY THE BREAKDOWN IS THE FEATURE, and a total is not. Under pressure a host
// does not need to know how big the document is; it needs to know WHICH PART,
// because that decides what it is allowed to release. Trimming history costs
// undo depth. Dropping the brick cache costs a stall. Dropping voxel or mesh
// content destroys the user's work and is never allowed. A single number cannot
// separate those, and the cheapest thing to release is precisely the one that
// was hardest to measure.
//
// WHY io. This is the only module the layering table lets name `scene`,
// `voxel`, `mesh` and `session` in one signature — which is exactly why
// `ClaySpaceDoc` already lives here. Each subsystem answers for itself; this
// header only adds up.

#include <cstddef>

#include "clay/io/clayspace.h"
#include "clay/memory/budget.h"
#include "clay/session/history.h"

namespace clay {
namespace io {

// A document's memory, separated by who owns it and by what it costs to let it
// go. `total` is the sum of every other byte field, asserted in code rather
// than trusted, so a field added later without being summed cannot pass
// silently.
//
// A FLOOR, NOT AN EQUALITY. These are container walks: allocator block headers,
// size-class rounding and arena fragmentation are invisible from here, as are
// this library's own code and static data. The number is always somewhat below
// what the OS charges the process, and a host comparing the two should expect
// the gap rather than read it as a leak.
//
// It is also LARGER than the same document's file, often several times: a
// `.clayspace` is RLE- and palette-compressed and a live voxel chunk is a flat
// array whether one cell is set or all of them.
struct MemoryReport {
    // -- the model. None of this may be released; it IS the user's work. -----
    std::size_t edit_list = 0;      // nodes, strokes, deformer chains, layers
    std::size_t voxel_content = 0;  // chunk storage across every level
    std::size_t mesh_layers = 0;    // imported geometry, unrecoverable
    std::size_t masks = 0;          // authoring state; small, but not nothing

    // -- droppable, in the order a host should reach for it ------------------
    // Undo for voxel layers. Held inside the grids, beside the content above,
    // and separated from it for exactly that reason: this is the only voxel
    // figure a host may act on.
    std::size_t voxel_sculpt_layers = 0;
    // The undo history and its journal. `clay_document_set_history_budget` is
    // the lever, and the only part of a document the engine will evict itself.
    std::size_t history = 0;
    // A thumbnail and camera bookmarks the document carries without
    // interpreting. Regenerable, and usually trivial — reported so the fields
    // sum, not because it will ever be the answer.
    std::size_t passthrough = 0;

    // -- transient: here now, gone when the gesture ends ---------------------
    // A mask copies its chunks on the first touch inside a recorded step, so a
    // mask costs roughly double for the duration of a stroke. Reported apart
    // from `masks` so a host sampling mid-gesture does not release something
    // else to make room for memory that was already going away.
    std::size_t transient = 0;

    // -- the surface tier ----------------------------------------------------
    //
    // ZERO UNLESS A HOST SAID OTHERWISE, and that is a statement about
    // ownership rather than an omission. A `clay_multires` and a
    // `clay_dynamic_sculptor` are OPAQUE and OWNING: the C ABI says so in
    // writing, and a host holds one beside its document rather than inside it.
    // So a document cannot walk them, and the roll-up would have lied by
    // reporting nothing at all for the largest thing an artist is holding.
    //
    // The seam is `document_memory`'s third argument: the host fills a
    // `memory::MemoryLedger` with `mesh::report_surface_memory` for each
    // surface it holds and hands it in. That keeps the ownership exactly where
    // the ABI put it — the document gains no reference to a surface it does not
    // own — and still lets one figure answer a memory warning.
    std::size_t surface_content = 0;   // adaptive surfaces: geometry and connectivity
    std::size_t multires_detail = 0;   // the coefficients: the wrinkles themselves
    std::size_t sculpt_layers = 0;     // a mesh layer stack's content
    std::size_t surface_caches = 0;    // chunk indices, evaluated levels, runtime caches
    std::size_t surface_scratch = 0;   // per-stamp working sets and preview staging
    std::size_t surface_undo = 0;      // vertex deltas and detail undo

    std::size_t total = 0;

    // -- the three a host can act on -----------------------------------------
    //
    // WHAT A MEMORY WARNING ACTUALLY ASKS. A single total is not the answer:
    // under pressure a host does not need to know how big the document is, it
    // needs to know WHICH PART, because that decides what it is allowed to
    // release. Derived from the fields above rather than stored beside them,
    // so a field added without being classified cannot make these disagree
    // with `total`.
    std::size_t essential() const;
    std::size_t rebuildable() const;
    std::size_t undoable() const;

    // -- what is here, for a host presenting the figure ----------------------
    std::size_t voxel_layers = 0;
    std::size_t mesh_layer_count = 0;
    std::size_t mask_count = 0;
};

// The whole document. `history` may be null, which is what a document that
// never enabled undo has, and reports zero rather than being an error.
//
// `surfaces` is the seam described on the surface-tier fields above: a ledger
// the host filled for the adaptive surfaces, hierarchies and layer stacks it
// holds BESIDE this document. Null is a document with none, and reports zero
// for them rather than guessing.
MemoryReport document_memory(const ClaySpaceDoc& doc, const session::History* history,
                             const memory::MemoryLedger* surfaces = nullptr);

// One layer, in the SAME struct. A host showing a per-layer list beside a
// document total should not have to reconcile two shapes.
//
// `history` and `passthrough` are document-wide and are therefore always zero
// here — documented rather than removed, so one struct serves both views.
//
// THE CONTENT LINES SUM EXACTLY ACROSS LAYERS AND THE EDIT LIST DOES NOT.
// Worth stating, because the tempting reading — "the layers add up to the
// document" — is false and a host would hit it.
//
// Content sums because every voxel chunk, mask cell and triangle belongs to
// exactly one layer id. The edit list does not, for two deliberate reasons:
// the document-wide figure includes overhead owned by no layer (the layer
// vector's capacity, the selection, the Document itself), and INSTANCE layers
// share one SdfContent, which the document counts ONCE — ten instances of one
// blockout are one allocation, and saying otherwise invites a host to free
// memory that does not exist — while each instance reports it in full, because
// displaying an instance costs an evaluation like any other layer and
// reporting zero would call it free.
//
// So a layer's edit_list is a CEILING on its contribution, not a partition.
//
// Returns false for a layer id the document does not have. NOT a zeroed report:
// a host reads that as an empty layer and shows a wrong answer confidently.
bool layer_memory(const ClaySpaceDoc& doc, scene::LayerId layer, MemoryReport* out);

}  // namespace io
}  // namespace clay
