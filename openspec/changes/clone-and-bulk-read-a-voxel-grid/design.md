## Context

A voxel layer's grid lives in `Document::voxel_layers` and crosses the ABI as a
borrowed handle that resolves its layer on every call. Converting it to a field
(`to_field`) is the expensive half of grid-to-field and is a const read, so it
can run on a worker — but only against a grid the worker can hold while the
interface thread keeps editing the document.

## Decisions

**Clone in the engine, not at the boundary.** `VoxelGrid::clone()` owns the rule
"a copy of the grid is not a copy of its session". Putting it in `clay_c.cpp`
would leave every other caller of the copy constructor — pyclay, the session
code — one member-wise copy away from writing into a history it does not own.
The copy constructor stays (the session layer copies grids by value on purpose,
for snapshots), and the header says when to use which.

**Close the recording rather than keep it.** A sculpt layer that is recording
in the source arrives closed in the clone, with its record intact. "The next
edit belongs to this pass" is a statement about the document's session; a
worker converting the clone does not edit it, and a host that does edit a clone
has not asked to extend a pass in a grid it never opened one on.

**The clone is undrawn.** Its dirty set is every occupied chunk, as
`deserialize` leaves a grid, rather than a copy of the source's pending set:
the source's set describes what the source's host has not drawn yet, which says
nothing about a grid nobody has drawn. `change_count` starts at 0 for the same
reason — it counts writes since construction.

**The bounds cache is left cold.** Copying a warm cache would be correct for
identical data, but the fields are copied while possibly being written by a
concurrent cold reader of the source; starting cold means the clone's answer is
always computed from the clone's own cells.

**Bulk read: active level, sorted.** Every other cell-addressed call acts on the
active level, so this does too, and another level is one `set_active_level` on a
clone away. The order is z, then y, then x — the order a box walk with x
innermost produces — so a host replacing its walk sees the same sequence. It is
produced by a sort rather than an ordered chunk walk (see the proposal's
measurements).

**Size query answers from the counters.** Both buffers NULL returns
`occupied_count()`, which for a whole level sums per-chunk counters and walks
no cell. A short capacity is `CLAY_ERROR_BUFFER_TOO_SMALL` with nothing written.

## Risks

- A host that clones a BORROWED grid on a worker while the interface thread
  reads its bounds races in the lazily mutating bounds cache. The header says so
  and gives both remedies (warm it first, or clone on the interface thread);
  nothing detects it.
- The bulk read allocates the whole cell list before copying it out: 16 bytes a
  cell, 1.4 MB at 89k cells, transient.
