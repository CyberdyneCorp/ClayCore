# Proposal: triangles straight to cells

## Why

An imported model reaches an SDF layer in **one** step and the voxel verbs in
**four**, paying for **two** samplings on the way (#117):

```python
mesh = clay.load_mesh("model.obj")
vol  = clay.Volume.from_mesh(mesh, cell=0.02)   # sampling 1: triangles -> band
doc  = clay.Document()                          # a document only to throw away
doc.add_sdf_layer("import").add(vol)
grid.rasterize(doc, bounds)                     # sampling 2: band -> cells
```

Each sampling places the surface within about half a cell of its own lattice,
so the second quantises a field that was itself quantised — and a feature that
survives the first can fall between centres on the second. The intermediate
document exists only to be discarded.

There is a second cost the ceremony hides. **`Volume::from_mesh` samples a
DISTANCE field and carries no colour at all**, so the detour arrives at the grid
with nothing to quantise and the model's colours are gone before the palette is
reached. Measured on the example's model: the direct path lands the source's
`(0.753, 0.353, 0.235)`, the detour a default grey.

## What

`VoxelGrid::rasterize_mesh` beside `rasterize_tape`, with
`VoxelGrid.rasterize_mesh` and `clay_voxel_rasterize_mesh` following.

- **Membership at the cell centre by the generalized winding number** — the same
  BVH machinery `mesh::to_field` owns, and the same sign
  `examples/19_mesh_import.py` exists to defend, applied ONCE instead of
  inherited through a band. Ray parity breaks on one hole and the
  closest-triangle pseudonormal is meaningless near an opening; dirty input is
  the input.
- **The region is optional**, defaulting to the mesh's bounds. `rasterize_tape`
  requires one because a document can be unbounded; a mesh cannot, so the
  natural default exists. An explicit region still bounds the work.
- **Colour from the mesh's vertex colours**, interpolated at the closest point
  on the nearest triangle and quantised to the palette by nearest entry, exactly
  as `rasterize_tape` quantises the tape's colour field. A mesh without colours
  takes one neutral entry.
- **The grid's contracts hold**: `change_count` moves only on cells that
  changed, the palette is shared, levels behave as they do for `rasterize_tape`.
- The same statement of what sampling preserves that `rasterize_tape`'s header
  carries, at the API rather than discovered.

### One supporting addition: `Bvh::closest`

Reading a colour needs to know WHICH triangle the nearest point landed on, and
the BVH could only report a distance. `Bvh::closest` returns the point, the
source triangle and its barycentrics; `unsigned_distance` becomes that query
with the answer thrown away and is implemented in terms of it, so there is one
traversal rather than two that could drift.

What it buys beyond this change is any **attribute transfer** off a mesh — a
uv, a normal, a colour — which nothing could do before.

## Not this

- **Not retopology or remeshing.** Occupancy sampling, nothing more.
- **Not a change to mesh layers.** A document-carried mesh stays
  never-evaluated; this is an explicit conversion a caller asks for, like every
  other bridge.
- **Not parallel.** The per-cell winding query is embarrassingly parallel and is
  listed in #119's inventory as such; this lands the serial path and the tests a
  parallel one would have to match.

## Impact

- **New:** `VoxelGrid::rasterize_mesh` (two overloads), `Bvh::closest`,
  `clay_voxel_rasterize_mesh`, `VoxelGrid.rasterize_mesh`.
- **Changed:** `Bvh::unsigned_distance` is now a thin wrapper. Results are
  unchanged, which the existing distance and winding tests assert.
- **Docs:** README's conversion section gains the direction; `docs/07`'s
  reachability table gains the row.
- **Example:** `48_mesh_to_voxels.py` with committed renders, measuring the
  direct path against the detour it replaces on a thick model, a thin fin and a
  holed model — and showing the colour the detour drops.
