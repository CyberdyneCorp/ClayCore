# Proposal: a subset mesh emits the triangles that straddle its boundary

## Why

Issue #66, from the host that built its incremental viewport on the key list
`close-webgpu-host-abi-gaps` added to `clay_brick_cache_mesh`. The subset it
returns is missing every triangle that straddles the requested set's boundary:
triangles produced by cells owned by *unrequested* bricks whose corners reach
into a requested brick. Measured on a unit sphere after one relief dab (80
dirty keys): the whole mesh holds 11421 triangles with at least one corner in
those bricks, the subset returns 10884 — 537 straddlers missing, 0 spurious.

Dilating the request does not work around it, and the issue proves that with
a table: at one, two, three and four rings of dilation their per-brick store
disagrees with a full rebuild in exactly 30 bricks — flat, because each ring
just relocates the boundary and mints a new set of straddlers. So *no*
sequence of subset calls can maintain a complete surface, and the only correct
host strategy is the whole-surface re-mesh the key list was added to avoid
(70 ms against 11 ms per dab on their 1043-brick model, growing with the model
rather than the edit).

## What changes

A subset mesh returns every triangle of the whole-surface mesh with at least
one corner inside a requested brick's closed box — the wholly-inside triangles
it already produced, unchanged, plus the straddlers. Each straddler is
attributed to the lexicographically lowest (x, then y, then z) requested key
whose closed box contains one of its corners, and lands inside that key's
reported ranges after the key's own cells, so the ranges still partition the
output and a per-brick host can dedupe (the issue's option 1, the one the
reporter asked for).

This is the new behaviour of the existing call, not a flag. The old
strictly-inside subset cannot maintain a surface, which means it served no
host correctly; keeping it behind a default would preserve a wrong answer as
the easy path. This is 0.x, the whole-surface path (`keys == NULL`) is
byte-for-byte untouched, and a caller that meshes every surface brick as its
subset still gets exactly the whole mesh, so the only callers who can observe
the change are the ones the old behaviour was failing.

Implementation: after marching a subset's own cells, the mesher marches the
one-cell shell of cells owned by unrequested *surface* bricks around the
request, keeps the triangles with a corner inside a requested brick, and
re-emits them through the same welding builder — so straddler vertices weld
onto the seam vertices the interior already made, and positions stay
bit-identical to the whole mesh's. Shell cells whose owner is not a surface
brick are skipped: the whole mesh marches no cell of theirs, and the subset
must stay a filter of the whole, never a superset.

## What this does not change

- The whole-surface path: `keys == NULL` marches the same cells and emits the
  same bytes as before. It has no boundary, so it has no straddlers.
- Triangle content: a straddler is the same triangle the whole mesh holds, at
  bit-identical positions. Nothing is invented; 0 spurious stays 0.
- The ranges contract: contiguous, in the order the keys were given, and a
  partition of the output.
