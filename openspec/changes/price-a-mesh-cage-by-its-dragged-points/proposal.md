# Price a mesh cage by its dragged points

## Why

`mesh::Lattice::displacement` summed every control point of the cage at every
vertex: `nx * ny * nz` multiply-adds whatever had been dragged. At the 32^3
ceiling that is 32,768 terms per vertex, and a host that previews a cage drag
by laying the cage over the mesh on every pointer move paid it on every frame.
ClaySpace measured one frame at ~1.7 s on a 62k-vertex mesh (4.7-5.2 s on an
80k-triangle fixture), against ~10 ms at 3^3, with a single corner in hand
(CyberdyneCorp/ClaySpaceDesktop#176).

The offset field is linear in the offsets, so a control point left at rest adds
exactly nothing to the sum. Its cost is work with no result.

## What changes

- The cage keeps the set of control points carrying a non-zero offset,
  maintained by `set_offset`, and `displacement` sums over that set alone.
  `is_identity` becomes that set being empty, in constant time.
- Each axis's Bernstein basis is built in O(n) from powers of `t` and `1 - t`
  with precomputed binomials, in double, instead of de Casteljau's O(n^2)
  recurrence in float — cheap beside the old n^3 sum, and not beside the new
  one.
- `dragged_count()` reports the size of that set, which is what one
  evaluation costs.
- `BM_MeshLatticeDrag` times one preview frame (apply with a record, then
  revert) at 3^3, 8^3 and 32^3 with one point dragged over ~100k triangles.

## Scope

`mesh::Lattice` only. No C ABI, Python or file-format change: every entry point
keeps its signature and meaning, and the results agree with the full sum to
float resolution. The SDF lattice deformer (`clattice_point`) is a different
evaluator, capped at 4 divisions an axis, and is untouched.
