# Add a reference host loop

## Why

The gallery exercises **234 of the 258** entry points the Python module binds.
The 24 it misses are not an oversight and will not be closed by adding more
example pages, because they are the wrong shape for one: an example is a
straight line, and every one of the 24 is about **sequencing**.

| bucket | unexercised |
|---|---|
| undo / session | `begin_undo_group`, `end_undo_group`, `undo_enabled`, `redo_depth`, `trim_stroke` |
| layer management | `move_layer`, `remove_layer`, `remove_mask`, `layer_protection`, `set_layer_protection`, `drop_level` |
| picking / input | `build_plane_pick`, `snap_to_surface`, `selection_bounds`, `contains`, `signed_distance`, `gradients` |
| live painting | `paint_cell`, `paint_mirrored`, `palette_set`, `erase_many`, `add_point`, `refresh`, `is_identity` |

`undo_enabled` only means anything after an edit has been grouped; `redo_depth`
only means anything after an undo; `trim_stroke` only means anything to a
stroke still being dragged. A page that shows one verb and renders it cannot
reach them, which is why the gap survived a gallery that otherwise covers the
library.

The second reason is external. The library is bound from Swift and C today and
the ROADMAP anticipates more hosts. What an implementor needs is not a list of
functions — the header has that — but the ORDER: when to open an undo group,
what a pick returns and what to do with it, which operations invalidate a
sculptor, what survives a save. That is currently spread across the C ABI
header, the device harness in Swift, and 58 example pages, and stated nowhere
as one sequence.

## What changes

A new `reference/` artifact: a scripted host **session** that opens a document,
picks against it, strokes with undo grouping, manages layers, paints, saves and
reloads — asserting an invariant at every step, and exiting non-zero when one
fails. It runs in CI beside the gallery.

It is written to be READ. Each phase is a named function with a comment saying
what a host is doing at that point and why the order matters, so a Swift or
Rust implementor can follow the sequence without running it.

## Non-goals

- **Not an application.** No window, no event loop, no input handling.
- **No rendering.** Measured before proposing this: `raycast_many` pins
  `find_backend("cpu")`, and a 320x240 primary-ray frame costs 55 ms at 200
  items and 867 ms at 3 000 — roughly linear in item count, the
  `add-item-spatial-index` problem. A viewport is a separate question and this
  change does not answer it.
- **No new dependency.** `pyclay`, `numpy` and the standard library, the same
  rule the gallery holds to.
- **Not a replacement for the gallery.** Examples show what a verb DOES; this
  shows what a host does AROUND them. Neither subsumes the other.
- **No new engine or binding code.** If a phase cannot be written against the
  current surface, that is a finding to record, not a licence to add API.
