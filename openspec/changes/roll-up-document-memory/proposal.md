# Proposal: what does this document cost?

## Why

A host on iOS is asked this question by the operating system, not by the user,
and it is not asked politely. `didReceiveMemoryWarning` arrives with no
argument and expects a response within a frame or two. The host must decide
what to release, and to decide it must know what it is holding.

This library cannot tell it. Every subsystem accounts for itself and **nothing
rolls up**:

- `clay_document_history_bytes` reports undo, redo and the journal — added by
  `add-history-budget`, and the only whole-subsystem figure that exists.
- `clay_voxel_sculpt_layers_bytes` reports one grid's sculpt layers, which is a
  *part* of one voxel layer of one document.
- `clay_brick_cache_stats` reports the evaluation cache, which is not owned by
  the document at all.
- Everything else — the edit list, the voxel chunk storage that the sculpt
  layers sit beside, masks, imported mesh layers, the thumbnail and camera
  passthrough blobs — reports **nothing**.

So the honest answer a host can assemble today is *"the history costs 4 MB and
one grid's sculpt layers cost 900 KB, and I have no idea about the rest."* The
rest is where the memory is. A rasterized 256³ voxel layer is the single
largest thing most documents hold, and it is invisible.

This matters more here than the missing number suggests, because the three
things a host would *want* to drop under pressure each have a different owner
and a different consequence:

- trimming **history** loses undo depth, which the user notices immediately;
- dropping the **brick cache** costs re-evaluation, which the user sees as a
  stall and nothing more;
- and **voxel and mesh content** cannot be dropped at all — it is the document.

A host that cannot separate those three will trim the wrong one, and the
cheapest thing to release is precisely the one nobody can currently measure.

## What changes

**One query that adds up.** A document-wide memory report with a per-subsystem
breakdown, in the shape `clay_history_bytes` already established so a host that
learned that surface learns nothing new.

The breakdown is the point. A single total answers "how big" and nothing else;
what a host needs under pressure is **which part**, because that is what
decides what it is allowed to release.

Reported per subsystem: the edit list, voxel content (separated from the sculpt
layers beside it, since one is the model and the other is undo), masks, mesh
layers, history, and the passthrough blobs.

**Per-layer, too.** A document-wide figure says the document is large; it does
not say the abandoned blockout layer from an hour ago is 200 MB of it. The same
report is available for one layer.

**Nothing is estimated silently.** Every figure is a walk of real containers —
`capacity()` where a container over-allocates, since that is what the allocator
is actually holding, not `size()`. Where a figure cannot be exact it is not
reported at all rather than guessed.

## What this is not

- **Not a budget, and not eviction.** `add-history-budget` gives the history a
  cap because history is *droppable*. Document content is not: evicting a voxel
  layer to save memory destroys the user's work. This change measures; what to
  do about the number is the host's decision and the history's budget is
  already the lever for the one part the engine may drop on its own.
- **Not the brick cache.** That cache is owned by an evaluator, not a document,
  already reports its own bytes, and already has a trim. Rolling it into a
  *document* figure would say a document owns something it does not.
- **Not an allocator hook.** Malloc overhead, arena fragmentation and the
  library's own code and static data are outside what a container walk can see.
  A host that needs the process footprint asks the OS, and this report is
  explicitly a subset of that number.
