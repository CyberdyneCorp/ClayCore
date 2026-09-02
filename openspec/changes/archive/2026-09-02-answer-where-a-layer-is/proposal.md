# Proposal: a layer should say where it is, whatever it holds

## Why

`clay_layer_bounds` reported bounds for an SDF layer and nothing for a voxel or
a mesh layer, however much material either held (issue #318):

```
bounds of an SDF layer  : Some(([-1 -1 -1], [1 1 1]))
bounds of a MESH layer  : None          <- attached from that same geometry
bounds of a VOXEL layer : None          <- 9,261 occupied cells
```

That is not a tight bound, it is a wrong one. A mesh cannot be unbounded — its
vertices ARE a box, and `clay_mesh_bounds` will give it — and a grid says where
it is itself, which `clay_voxel_bounds` already answers.

The old behaviour was DELIBERATE and written down: a unit test asserted
`has_bounds == 0` for a mesh layer, with "clay_layer_bounds is derived from SDF
shapes and is deliberately left meaning what it always meant". That is a
decision about the implementation dressed as a decision about the contract.
`scene::Layer` holds only SDF content because `check_layering.py` withholds
`clay/voxel` and `clay/mesh` from `clay::scene` — a real invariant, and not a
reason for the ABI to answer a question wrongly when the document that owns all
three representations is right there.

## What Changes

`clay_layer_bounds` answers from whichever representation the layer holds. A
voxel layer answers from its occupied cells, a mesh layer from its vertices, and
both compose with the layer transform exactly as the SDF arm already composes it.

The composition happens at the C ABI, where the document owning the three side
tables is in scope. `pick::layer_bounds` is unchanged and stays SDF-only, so the
layering invariant is untouched.

A layer holding no material still reports 0. An empty grid is genuinely nowhere,
which is a different answer from "this kind cannot say".

## Impact

**It unblocks a documented conversion.** `clay_voxel_rasterize_mesh` needs a
region, and a host had no way to supply one for a mesh layer except by reading
every vertex back — for a 300k-triangle model, a copy of every position to
answer a question the engine can answer from data it already holds. One of the
six conversion directions the ABI documents was unreachable through the ordinary
path, and reported as uncovered on every benchmark run. Measured after the fix:
a mesh layer's bounds drive `clay_voxel_rasterize_mesh` to 33,552 occupied cells.

**Anything that asks a layer where it is inherits it**: framing a camera on a
mesh layer, placing a manipulator, deciding what region an edit reaches.

**A BEHAVIOUR CHANGE, not an addition.** No symbol moves and no signature
changes, so nothing recompiles — but a caller that branched on `has_bounds == 0`
to mean "this is a mesh layer" now takes the other branch. That reading was
never sound (an empty SDF layer reports 0 too) and it is called out in the
release notes.

## Non-goals

**`clay_layer_selection_bounds`.** It takes node ids, and a voxel grid and a
mesh layer have no nodes to select. It stays SDF-only, which is not the same
defect: it answers the question it is asked.

**Meshing or evaluating the content.** A bound is read off what is stored — the
occupied-cell extent and the vertex positions — and a mesh a document carries
stays never-evaluated.
