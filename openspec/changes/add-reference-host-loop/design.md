# Design

## Where it lives

`reference/host_loop.py`, not `examples/`. The gallery has one page per feature
and a runner that renders and contact-sheets them; this is one session that
renders nothing. Filing it under `examples/` would put it in `run_all.py`'s
list, where it would be the only entry producing no image, and would muddle a
capability whose spec is "every feature area has a runnable example".

A new `reference-host` capability rather than a requirement bolted onto
`examples`, because the two make different promises: the gallery promises
COVERAGE of the vocabulary, this promises the ORDER. The new capability also
has to be added to `CAPABILITY_EXAMPLES` in `run_all.py`, whose gate fails on a
living capability with no entry — the interlock is deliberate.

## Shape

Phases as named functions, run in sequence by `main()`, each returning nothing
and raising `SystemExit` with a message naming the phase and the broken
property. Assertions rather than prints are the point: a session that only
printed would rot silently the way an export-only I/O check does.

```
open_document      -> a document with an SDF, a voxel and a mesh layer
pick_the_surface   -> snap_to_surface, build_plane_pick, selection_bounds,
                      contains, signed_distance, gradients
stroke_with_undo   -> begin/end_undo_group, undo_enabled, redo_depth,
                      undo/redo, trim_stroke, Stroke.add_point
manage_layers      -> move_layer, remove_layer, set_layer_protection,
                      layer_protection, drop_level, remove_mask
paint_live         -> paint_cell, paint_mirrored, palette_set, erase_many,
                      refresh, is_identity
save_and_reload    -> round trip, and the invariant that survives it
```

## Why this order

It is the order a host is forced into, and each step depends on the one above:
a pick needs geometry to hit, an undo group needs an edit to group, a layer
reorder needs more than one layer, a save needs something worth saving. Writing
the phases in dependency order is what makes the file readable as a sequence
rather than as a list of calls that happen to be in one file.

## Assertions over renders

Every phase asserts a property that would survive a rewrite of the internals:
undo returns the document to the cell set it had, a protected layer refuses the
edit, a reloaded document evaluates identically. Where an exact comparison is
possible it is used rather than a count — a count matches for the wrong reasons
often enough to be worth not trusting, which is the same reason the sculpt-layer
removal check compares cell sets.

## Coverage interlock

The session carries its own check, reading the 24 names from a list in the file
and requiring each to appear in it — the same shape as
`15_voxel_verbs_and_repair.py`, and for the same reason: a name-matching gate
has to be able to fail. Widening it to read from `dir()` is deliberately NOT
done here, because the sequencing set is a curated claim about which entry
points are order-dependent, not everything the module binds.

## Rejected

- **A tkinter viewport.** Ships with CPython, so it would not break the
  dependency rule — but the measured frame budget makes it a slideshow at any
  realistic document size, and the non-goal exists to stop this change turning
  into the app question.
- **Driving the C ABI via ctypes** to be closer to what Swift sees. It would
  duplicate the binding layer for no gain: the sequencing being documented is
  identical, and the Python surface is the one a reader can follow.
