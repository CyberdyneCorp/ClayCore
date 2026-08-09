# Proposal: let a host see whether a voxel edit did anything

## Why

`clay_voxel_sculpt_grab` resamples nearest-cell, and the rounding is per axis.
A displacement under half a cell on every axis therefore resolves each cell to
itself, writes back the index already there, and returns `CLAY_OK`. That is
correct — the call did exactly what it was asked — and it is also what a drag
tool looks like when it is fed raw pointer deltas: at a `voxel_size` of 0.12
every per-sample delta of a slow drag is sub-cell, every call succeeds, and
nothing moves. The host has no way to tell that from a drag that worked.

Nor is the blind spot grab's. `sculpt_smudge` rounds the same way and has the
same dead zone. `flatten` and `scrape` have no rounding trap and the same
invisibility: an `offset_cells` nudge under a cell, an already-flat region, or
a footprint over empty space all report success and change nothing. `smooth`,
`pinch`, `magnify`, `inflate`, `fill_cavities`, `carve_alpha`, the repairs and
the dithered falloff brushes are all in the same position.

`clay_voxel_occupied_count` is not a workaround. Grab and magnify conserve
material — the C ABI test already says so in as many words — so the count is
identical whether a lump moved a whole cell or did not move at all.

## What it is

One grid-level counter: cell writes that actually changed a cell, since the
grid was constructed. Monotone, never reset, only differences meaningful.
`VoxelGrid::set` is the sole mutation funnel for the whole class — every brush,
fill, sculpt verb, repair and tape rasterization routes through it, and the
only writes that bypass it build a fresh grid in `deserialize` — so one
comparison inside `set` instruments every verb at once, at the cost of one
`uint8_t` compare on a path that already reads and writes that byte.

It crosses the ABI as `clay_voxel_change_count`, in the queries block, and
reaches pyclay as the read-only `VoxelGrid.change_count`.

Two guarantees of deliberately different strength, both stated in the header:
`delta == 0` exactly when the grid is byte-identical to before the bracketed
calls (universal, because of the funnel); and `delta` is the exact number of
changed cells for every verb that writes each cell at most once, which is all
of them except `pinch` and `magnify`. Those two clear a cell and write its
colour into a neighbour the same call may visit later, so for them it is an
upper bound. Documented rather than papered over.

## What it is not

**Not a parameter on `clay_voxel_sculpt_grab`.** The signature is published and
stays as it is.

**Not a `clay_voxel_sculpt_grab_counted`.** `close-c-abi-issue-gaps` already
ruled on this shape: `clay_mesh_load` gained a parameter rather than acquiring
a `_budgeted` sibling, because "two entry points that differ only by one
nullable argument would be two ways to say one thing, which this codebase
avoids deliberately". A sibling per verb would be eleven of them, plus one for
every verb added later.

**Not a new `clay_result` value.** `clay_result` is `CLAY_OK` plus eight
errors and the header states the enum is ABI-stable. An existing entry point
returning a new non-zero value would make every already-compiled caller's
`if (r != CLAY_OK)` treat a successful no-op as a failure — a behavioural break
dressed as an additive one. The codebase has already decided this question for
a rejected brick submit: an ordinary outcome of an interactive session arrives
through an out-parameter and the call still returns `CLAY_OK`, "the same choice
`clay_document_undo` makes for 'nothing to undo'".

**Not "the last edit changed N".** Hidden per-grid state with an ambiguous
"last", useless across a batch. A monotone counter composes: a host brackets
one sample or a whole drag gesture, as it likes.

**Not accumulation inside the verb.** The verb is stateless and the grid has no
idea where one gesture ends. `clay_voxel_size` gives the host the cell size it
needs to accumulate a drag against, which is the only place that knows.

**Not `out_applied`'s unit.** The three entry points that carry an
`out_applied` count stamps run or items warped; this counts cells changed. The
header says so, so the two are not read as one.

## Documentation

The grab block in `clay.h` gains the sharp version of the dead zone: rounding
is per axis, so a pull of 0.4 cells on each of three is 0.69 cells long and
still moves nothing; half a cell on the largest component is necessary, not
sufficient, because the falloff shrinks the pull away from the centre and
`front_only` halves it at the centre outright — the front gate is 0.5 on the
plane through the centre, so the dead zone there is twice as wide. The
sculpting-verbs preamble states once, for all of them, that a valid call with
no effect is normal and reports `CLAY_OK`. Smudge is marked as sharing grab's
rounding.
