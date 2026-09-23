# Design

## One kind, not a third mechanism

The edit's record half has the shape a merge-down's undo already has — truncate
a record to a known length and put back some overwritten `after` values — so a
Pass is a `SculptLayerOp` and replays through `apply_sculpt_layer_op`, the
`VoxelLayerProperty` step kind and its journal event. The field names are the
merge's (`lower_count`, `lower_afters`, `held.changes`) plus `pass_afters`, the
value each rewritten entry was left at, which a merge never needs because its
redo re-runs the fold.

## How the record half is captured

`VoxelGrid::set` is the choke point, and its recording hook is where a record
changes, so the capture lives there: a third channel beside the change sink,
null when off. A first touch appends and is recovered at close as the tail past
`lower_count`; a rewrite of an older entry notes `(index, previous after)`. At
close the notes are deduplicated keeping the FIRST (the value the edit found),
rewrites that ended where they began are dropped, and the new values are read.
The grid still knows nothing about steps; `History` decides.

## Why a no-op edit's record entries are dropped

The recording hook lists every cell a pass TOUCHED, including writes that
changed nothing, while a step exists only for an edit that changed a cell. The
first cut left those entries in the record when the step was dropped. That is
not benign: a Pass records positions, so after undoing an earlier Pass (which
truncates past them) and redoing it (which does not bring them back) every
later Pass found a record of a different length from the one it recorded and
was refused — and a journal, which never saw the dropped edit, refused on
replay. The test "an edit inside a layer that changed nothing leaves no trace"
fails exactly that way with the rollback removed.

Recording the no-op edit as a step instead would be an undo that changes
nothing on screen, the defect every recorder here exists to avoid. So the edit
is dropped and its record entries are rolled back with it. With undo off
nothing captures, and the record lists them as before; `sculpt_layer_cell_count`
is documented as "how many cells the pass changed", which the undo-on behaviour
matches more closely, not less.

## Recording state across undo and redo

Undoing a creation must end recording — the layer being recorded into is gone.
Redo brings the layer back CLOSED rather than reopening it: recording is not
saved, it is a gesture's state, and `end_sculpt_layer` is not a step, so a redo
that reopened it would have a host that had already ended the pass refused at
its next `begin` ("a sculpt layer is already recording").

## Strictness

A Pass replays only onto a record of exactly the length it left (undo) or found
(redo), and a Begin undoes only the top layer with an empty record. Anything
else is refused before a cell moves, like every other layer operation.
