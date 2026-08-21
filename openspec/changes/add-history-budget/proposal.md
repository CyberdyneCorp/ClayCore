# Proposal: bound the undo history

## Why

`UndoStack` holds `std::vector<Entry> undo_` and `std::vector<Entry> redo_`.
There is **no cap of any kind** — no depth limit, no byte accounting, no
eviction, no way for a host to ask what the history costs or to tell the engine
it costs too much. The only lever is `enable_undo(false)`, which is not a lever,
it is a light switch.

That was survivable while the sessions were short. It is not survivable on the
device this library is being handed to. An iPad at 120 Hz Pencil input feeds
the stroke engine for hours, and iOS does not warn twice before it kills a
process for memory.

Two things make the size hard to predict rather than merely large:

- **The expensive entries are the inverses of removals.** The undo stack stores
  inverses, so removing an item records an `AddNodeCmd` carrying a whole `Node`
  — 440 bytes plus its deformer chain and stroke points, against 8 bytes for
  the `RemoveNodeCmd` that a plain add records. A session of adds is cheap and
  a session of deletes is not, and nothing tells the host which one it is in.
- **Coalescing already helps, and only for one shape.** Consecutive
  `AppendStrokeCmd`s on the same node merge, which is why a long stroke is one
  step. Stamps that create a new node each — the ordinary sculpt path — do not
  coalesce, so a stroke of N stamps is N entries.

A host cannot work around this. It cannot measure the stack, cannot trim it,
and cannot cap it. Every host will therefore either leave undo unbounded and
risk the kill, or disable it and ship without undo.

## What changes

A memory contract on the history, in the same shape the brick cache already
has (`clay_brick_cache_trim` and its stats), because a host that has learned
one budget surface should not have to learn a second.

- **Ask what it costs.** Bytes held by the undo and redo stacks, and the depth
  of each, as one query.
- **Set a budget.** A byte budget on the history. When it is exceeded, the
  OLDEST entries are dropped — the undo stack loses depth at the far end, never
  the near end, so the next undo always works.
- **Trim on demand,** for a host that has just been told by the OS that memory
  is short, rather than waiting for the next edit to trigger eviction.
- **Say what a dropped entry means.** A truncated history is not an error and
  must not read as one: the host needs to know the horizon moved so it can grey
  out a menu item rather than let a user hunt for a step that is gone.

## What this is NOT

**Not history compression, and not checkpointing.** Both were on the table.
Compression trades CPU on the interactive path for memory, and this library has
not measured that trade; checkpointing means periodically snapshotting the
document so the history can be rebuilt, which is a different feature with a
different failure mode. A budget with eviction is the smallest thing that
removes the unbounded allocation, and it is the thing to build first because it
makes the other two measurable.

**Not an undo that spans representations.** Voxel and mesh edits are outside the
command vocabulary — see `correct-the-undo-scope`. This bounds the history that
exists; it does not enlarge it.
