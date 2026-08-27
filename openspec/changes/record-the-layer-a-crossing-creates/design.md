# Design

## The side map survives the undo, deliberately

`AddLayerCmd` carries a `scene::Layer` by value. A `scene::Layer` cannot carry a
`voxel::VoxelGrid` — the layering table puts `voxel` above `scene`, and a
command that carried megabytes of cells would make every layer removal cost the
sculpt it held.

So undoing a voxel layer's creation removes the LAYER and leaves the grid in
`ClaySpaceDoc::voxel_layers`, keyed by the id. This is exactly what the mesh
path has done since `add-mesh-layers`, and it is what lets a redo pick the cells
back up rather than restoring an empty layer.

The consequence is an orphaned map entry, which is fine in memory and must not
reach a file — see below.

## A group is one step, across representations

`UndoStack` collapses the commands inside a bracket into one entry, and
`sync_scene_steps` reconciles that to one Scene step. Nothing did the same for
the kinds `UndoStack` cannot see.

`Step::Kind::Compound` holds the steps a bracket produced and applies them
backwards on undo, forwards on redo. Three decisions are load-bearing:

**The Scene child is first, and there is at most one.** An open group occupies a
single `UndoStack` entry however many commands land in it, so `sync_scene_steps`
appends exactly one Scene step when the bracket closes. It is stored first
rather than in the order it was reconciled, because a Voxel, Mask or Mesh child
names a LAYER and the Scene child is what creates or removes it — undoing scene
last, and redoing it first, keeps every payload applied while the layer it names
is present.

**A refused child leaves the whole step unapplied.** A Voxel step whose layer
cannot be resolved refuses rather than skips — that rule predates this change
and is why an undo does not silently reverse something older than the user
asked for. A compound inherits it: the children already moved are put back and
the step stays on the stack. Half a compound is precisely the state this change
exists to remove.

**A bracket holding a barrier is not folded.** A barrier is the horizon a host
draws. Folded into a reversible compound it would offer an undo straight across
the thing that says you cannot go back.

## Eviction cannot run under an open bracket

`enforce_budget` evicts from the oldest end, which would shift `group_start_`
out from under an open group and make the fold take the wrong steps. It returns
early while `grouping_` is set, and `collapse_group` calls it explicitly once
the fold is done — so the budget is applied at the same edit it always was,
one step later in the sequence.

## The journal needs no version bump

`Step::Kind` is never serialized; only `JournalEvent::Kind` is. Replay
re-performs `GroupBegin` / … / `GroupEnd` through the same entry points a live
session uses, so a recovered session rebuilds the same Compound on its own.

## The voxel orphan filter

`save_clayspace` filtered orphaned MESH chunks and not voxel ones. That was
recorded as a known inconsistency at the time (`add-mesh-layers` tasks 7.7: "the
mesh rule makes the inconsistency visible and a follow-up can close it") and was
unreachable in practice, because nothing removed a voxel layer while leaving its
grid.

Recording the creation makes it reachable and turns it into a corruption path:
`deserialize_document` derives `next_layer_id_` from the layers PRESENT, so ids
are no longer monotonic across the gap a removed layer leaves. A saved orphan
can be captured by the next voxel layer to take that id, which then comes up
holding a dead sculpt. The filter closes it on both sides — never written,
dropped on load — and `insert_or_assign` at the creation site makes the
in-memory path safe against a file that predates the filter.

## Known limits

- Journal replay of a voxel-layer creation restores the layer record but not its
  grid: `voxel_size` is not expressible in `scene::Layer`, so a `Kind::Voxel`
  event that follows is still refused rather than applied to a grid that does
  not exist. Unchanged by this work, and now visible because the creation is on
  the journal at all.
- Creating a MASK (`clay_document_add_mask`) is still not a command. Masks
  record their EDITS and not their existence, which is the same shape of gap
  this change closes for voxel layers.
- `Document::instance_layer` inserts without a command. No binding reaches it,
  so it is latent rather than a defect.
