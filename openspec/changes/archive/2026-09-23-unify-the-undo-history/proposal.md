# Proposal: one undo, across three representations

## Why

`correct-the-undo-scope` found this and wrote it down. Nothing fixes it:

| representation | mechanism | reaches `clay_document_undo`? |
|---|---|---|
| SDF edit list, layer properties | `UndoStack` over `Command` | yes |
| voxel grid | sculpt layers — record a pass, dial it, merge it down | **no** |
| mesh layer | `mesh::VertexDeltas::revert` | **no** |

Each is defensible alone, and the reasons are good: a voxel edit has no compact
inverse — the inverse of "carve here" is the cells that were there — which is
exactly why the voxel side records passes; a mesh displacement is not an edit
item, so `scene::Command` has no variant for one. **But no single user-visible
undo spans them**, and the proposal that found this says how a host learns:
"by shipping."

**This is the difference between a modelling kernel and a sculpting app.**
ZBrush, 3DCoat and Blender each have one Ctrl+Z. A host built on this library
must either offer three different undo affordances depending on which layer is
selected, or offer one that silently does nothing for two of the three. And
this library's own flagship example — `examples/42_representation_round_trip.py`
— is a workflow that crosses representations mid-sculpt: block out in SDF,
rasterize to voxels, smooth the seam with a voxel verb, convert back. That is
precisely where a user reaches for undo, and precisely where it is least likely
to do what they mean.

It also blocks crash recovery. A journal of applied commands would recover an
SDF sculpt and silently drop every voxel and mesh edit — a recovery that looks
like it worked and lost half the model, which is worse than no recovery at all.
So this comes first.

## The finding that makes it tractable

**Every inverse already exists.** The obstacle was never the math.

- `VoxelGrid::SculptLayerRecord` stores `SculptChange{cell, before, after}` for
  every cell a pass touched, *in the order the pass touched them*, which is
  exactly what a reverse replay needs. `VoxelGrid::revert_from` and
  `apply_from` already do that replay — they are private, and they exist to
  serve sculpt-layer visibility and strength.
- `mesh::VertexDeltas::revert(Mesh&)` is already an undo, is already public,
  and is already refused against a mesh of the wrong vertex count.
- `UndoStack` already holds command inverses, already coalesces a stroke and
  already groups.

What is missing is not an inverse. It is an **order across the three** — a
record of which representation was edited when.

**And there is already one object that owns all three.** `io::ClaySpaceDoc`
holds the scene document, the voxel grids, the mask fields and the mesh layers.
A session history has a natural home and needs no new ownership.

## What changes

**A session history ABOVE the three mechanisms, not a merge into `Command`.**

Forcing a voxel pass into `scene::Command` is the thing each mechanism rejected,
for reasons that have not changed: `Command` is a small serializable variant and
a voxel pass is an unbounded list of cells. So the history is an ordered log of
STEPS, each naming the mechanism that owns it and carrying the token that
reverses it — a command inverse, a recorded pass, a vertex delta. Undo pops the
newest step whatever produced it, and dispatches.

- **One step order across representations.** An SDF stamp, then a voxel smooth,
  then a mesh grab: three undos take them off in that order, and three redos put
  them back.
- **Each mechanism keeps its own design.** `UndoStack` keeps its coalescing and
  grouping. Sculpt layers stay addressable — dialling a layer's strength is
  still a thing a user does out of order, and it becomes a step of its own
  rather than being confused with the pass that created it.
- **`clay_document_undo` keeps working** and gains the two representations it
  was silently missing. A host that already calls it gets more correct
  behaviour, not a different API.
- **Existing per-representation surfaces stay.** A host that wants to address a
  sculpt layer directly still can; the session history is the *user's* undo, not
  a replacement for the *artist's* layer stack.

## What this is NOT

**Not a merge of the three storage mechanisms.** They stay exactly as they are.
This adds an index over them, and the moment it tried to be more than that it
would be re-litigating three design decisions that were each right.

**Not undo for everything.** Operations outside all three vocabularies — a
consolidate, a rasterize, a conversion between representations — are not made
undoable by this change. Where a step cannot be reversed, the history SAYS so
rather than offering an undo that does nothing. Naming the boundary honestly is
half the value; `correct-the-undo-scope` exists because the last one was not
named.

**Not the memory budget.** `add-history-budget` bounds the undo stack's
allocation and is unbuilt. This change widens what the history holds, which
makes that row *more* urgent, and the two must agree about what a byte count
covers — but bounding is that change's job, not this one's.

**Not crash recovery.** That is the next change, and it is the reason this one
is ordered first.
