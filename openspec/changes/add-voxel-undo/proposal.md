# Proposal: Voxel undo (issue #6, part 1)

## Why

Every `clay_voxel_set_brush` / `clay_voxel_erase_brush` / `clay_voxel_paint_brush`
call mutates the grid directly, outside the command vocabulary. A host
sculpting UI (ClaySpace) has SDF strokes, primitive placement, restyles,
deletes, and layer operations all undoing as clean steps — and voxel
stamps silently exempt. To users this reads as a bug: three-finger-tap
undoes the last smooth stroke *through* the blocks they placed after it,
because the voxel edits never entered the history at all.

## What

Voxel brush edits on **document-borrowed grids** become undoable steps in
the same history the scene commands use, with per-drag coalescing.
Standalone grids (`clay_voxel_grid_create`) stay direct — they have no
document and no history.

## Constraint that shapes the design

Voxel grids live in `io::ClaySpaceDoc.voxel_layers`, deliberately outside
`scene::Document` ("the scene module stays voxel-agnostic by layering
rule"). A `VoxelBrushCmd` in `scene::Command` would force the scene module
to know voxel types — breaking that rule — so the vocabulary is the wrong
home unless the layering rule itself is revisited.

## Options considered

1. **Scene-level `VoxelBrushCmd`** — cleanest ABI story (everything is a
   command; serialization for free), but breaks the voxel-agnostic rule
   and drags `voxel::` types into `scene::commands.h`. Rejected unless the
   layering rule is consciously retired.
2. **Interleaved journal above `scene::UndoStack`** (recommended) — the
   document keeps a step journal where each entry is either "one scene
   UndoStack step" or "one voxel diff" (sparse cells with before/after
   palette slots). `clay_document_undo/redo` pops the journal: scene steps
   delegate to `UndoStack`, voxel steps apply their inverse diff to the
   grid. Lives in the io/binding layer; scene stays voxel-agnostic;
   `begin/end_undo_group` brackets and per-drag coalescing (consecutive
   brush diffs merge, keyed the way `AppendStrokeCmd` coalesces) work at
   the journal level. Cost: the journal must stay 1:1 with `UndoStack`
   entries — an invariant to test hard.
3. **Second undo stack for voxels only** — trivial, but undo ordering
   across modes becomes wrong (undo must pop the *most recent* edit
   regardless of kind). Rejected.

## Scope

- Journal + diff capture for the three brush verbs on borrowed grids.
- Coalescing: brush calls between `clay_document_begin/end_undo_group`
  are one step; outside groups, consecutive same-verb calls do NOT
  coalesce (the host brackets drags — ClaySpace already does for strokes).
- `clay_document_undo_state` depths count journal steps.
- Out of scope: voxel diffs in the `.clayspace` command chunk (files
  already persist grids whole in `VOXL`), masks, scene enumeration
  (issue #6 part 2 — separate change).

## Host impact

ClaySpace removes its "voxel edits are not undoable" caveat and its
mirror-side guard; its op-log gains a `.voxelStep` case that pairs 1:1
with journal steps, exactly like every other command today.
