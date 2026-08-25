# Proposal: refit the mesh BVH instead of rebuilding it

## Why

`MeshSculptor::refresh_bvh()` throws the tree away and calls `Bvh::build` again.
Measured on this desktop, single-threaded:

| triangles | vertices | `Bvh::build` |
|---:|---:|---:|
| 130,050 | 65,536 | 32.0 ms |
| 2,093,058 | 1,048,576 | 628 ms |
| 4,187,618 | 2,096,704 | **1,341 ms** |

Against the dab it sits beside — 0.251 ms for a seeded-geodesic stamp moving 188
classes on that same 2M-vertex mesh — **the rebuild is 5,300x the sculpting.**

The header already knows, and the workaround it chose is the only survivable one
today:

> Positions move under it: a sculpted mesh reports the surface as it was until
> `refresh_bvh` runs. That is the caller's call to make — refitting per stamp is
> the expensive half of a stroke, and a brush that keeps its depth from the
> stroke's first pick is usually what an artist wants anyway.

So a host has two options and no third: never refresh and let picking drift
(6.9e-4 off the ray in `reference/host_loop.py`, 4.4e-2 in this change's own
test — fine for a brush and
the whole budget for a gizmo), or refresh and wait 1.3 seconds.

**A mesh layer's topology is fixed.** That is the representation's defining
property and the one this change spends: the tree's SHAPE stays correct when
vertices move, and only the bounds are stale. Recomputing bounds for the
triangles that actually moved, and the ancestors above them, is proportional to
the brush rather than to the mesh.

## What changes

- `Bvh::refit(mesh, changed_triangles)` — update the bounds and the
  winding-number summaries for a named set of triangles, bottom-up.
- `Bvh::refit(mesh)` — the same for every triangle, for a global deformation.
- `Bvh::quality()` — a surface-area measure of how far the tree has drifted from
  a tree built for the current positions, so a host (or `MeshSculptor`) can
  decide when a rebuild has become worth it. A refit keeps the tree CORRECT
  forever; it does not keep it FAST forever.
- `MeshSculptor::refit_bvh()` — refit from the classes the last stamp moved,
  which the sculptor already knows.

## What makes it possible, and what it costs

Three things the tree does not carry today:

- **`source` to slot.** `Tri::source` names the mesh triangle, but the build
  reorders `tris_` with `nth_element`, so finding a triangle means scanning.
- **Slot to leaf.** A changed triangle has to reach the node holding it.
- **A parent link**, to walk from a leaf to the root.

And one that is a change in kind rather than an addition: **`summarize()` is
O(span)** — it loops over every triangle beneath the node. A bottom-up refit that
re-summarised each ancestor would rescan the whole mesh at the root and buy
nothing. The summary has to become *combinable*, so a parent is the sum of its
two children.

That is cheaper than it looks. `summarize` already computes the total area and
the area-weighted centroid sum and then discards both, keeping only their
quotient. Storing them makes the parent step O(1).

## What this does NOT do

**It does not make the first build cheaper.** A freshly imported 2M-vertex mesh
still pays 1,341 ms on its first pick, because there is no tree to refit. That
is a real gap and a different fix — the build is single-threaded — and it is
recorded rather than folded in here.

**It does not make refit bit-identical to a rebuild.** The boxes are, because a
union of unions is the same union and min/max do not round. The summaries are
not: floating-point addition is not associative, so summing two subtree totals
differs in the last bits from summing a span. The requirement is agreement to
tolerance and CONSERVATIVE bounds, and the tests say which is which.
