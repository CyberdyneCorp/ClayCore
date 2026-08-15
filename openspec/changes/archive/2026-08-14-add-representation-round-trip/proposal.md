# SUPERSEDED by `voxel-to-field` (retired 2026-08-14)

**Do not implement this. It was delivered, under different requirement names
and with one of its open questions answered the other way.**

This plan was written before `voxel-to-field`, which shipped the capability it
describes: `VoxelGrid::to_field`, `clay_item_volume_from_voxels`,
`clay_voxel_to_layer`, and `examples/42_representation_round_trip.py` — the
round trip this proposal is named for, running end to end. Its tasks stayed
unticked, so it read as unstarted backlog for months while the work sat merged.

Where each of its intents ended up:

| this plan's requirement | delivered as |
|---|---|
| A grid converts directly to a field | `voxel-engine` — *A grid converts to a field without a mesh in between* |
| Conversion is lossy, and says so | `voxel-engine` — *The SDF-to-voxel direction states what it guarantees* |
| Conversion across the ABI | `c-abi` — *A host converts a sculpt into a layer it can keep working on* |
| Conversion is an ordinary edit | **NOT delivered, and deliberately** — see below |

**Task 1.1 was the real question and it was answered the other way.** It asked
whether conversion happens IN PLACE or produces a new layer beside the original.
The answer is a new layer: `clay_voxel_to_layer` adds one through
`AddLayerCmd` — so it is undoable like any other layer edit — and leaves the
grid untouched. The scene-model requirement below describes in-place conversion,
which is the road not taken, and is why this change is archived with its spec
deltas SKIPPED rather than applied: applying them would write a requirement the
library deliberately does not meet, plus three duplicates under new names.

The reasoning for that choice is in the README's "Converting between them":
once rasterized, the parametric items behind a sculpt are no longer reachable
from the voxel side, so the return trip hands back a new layer rather than
destroying the original.

---

# Proposal: move a sculpt between the two representations

## Why

The engine has two representations and the bridge between them runs one way.

SDF to voxel is direct: `VoxelGrid::rasterize_tape(tape, region)`. Voxel back
to SDF exists only as a detour — mesh the grid, run `mesh::to_field`, place the
result as a volume item — which resamples onto a lattice frozen at bake time,
discards the palette, and hands back an operand rather than something you can
keep sculpting.

Each representation owns half a toolkit. The SDF side has primitives, booleans,
blends and deformers, and no sculpting verb at all. The voxel side has ten
sculpting verbs, masks and repair, and no booleans-with-blends and no
resolution independence. So a sculptor picks one and lives inside its half.

The natural workflow is to use both and to keep moving: block out with
primitives and booleans because that is what they are good at, hop to voxels to
push the forms around freely, hop back to boolean a hard-surface part against
the result, and carry on. Today that is a one-way trip with a lossy return.

Both examples show the cost. `34_organic_character.py` is SDF, so the arm/torso
seam cannot be smoothed — the toolset has no smooth. `35_hard_surface_helmet.py`
is SDF for the same reason, so every plate is a boolean assembly rather than
something that could be trimmed and sculpted. Neither could reach for the other
half without leaving the sculpt behind.

## Approach

Make the return trip first-class:

- **Voxel to field, directly.** Without going through a mesh — the grid already
  knows a step-function field (`-voxel_size/2` inside, `+voxel_size/2` outside);
  what is missing is a proper narrow-band signed distance built from it, so the
  result is usable as an operand with a real Lipschitz bound rather than a
  staircase.
- **Round-trip fidelity stated, and enforced.** Sample size and band width are
  the caller's, and the guarantee that the shape survives within them should be
  a scenario a test holds rather than a hope.
- **Colour survives.** The palette is real authored data; a round trip that
  drops it makes the trip unattractive regardless of the geometry.

The SDF-to-voxel direction already exists and mostly needs its guarantees
written down rather than new code.

## The honest catch

A round trip is **not** lossless in either direction and the proposal must say
so rather than imply otherwise.

Going to voxels quantises to a lattice: a boolean's sharp edge becomes a
staircase at the cell size, and no amount of care recovers the original
crispness on the way back. Going to a field from voxels turns a binary
occupancy into a distance, and the band width decides how much of the field is
meaningful.

That makes the honest framing a **conversion**, not a view. The design has to
say what is preserved (the surface, within a stated tolerance; the colour) and
what is not (exactness, and the procedural history — once converted, the items
are gone and the parameters are no longer editable).

Whether the procedural history should be *kept alongside* the converted result,
so a host can offer "go back", is the significant open question and probably a
change of its own.

## Open questions

- Whether conversion is destructive or produces a new layer beside the original.
- Whether the palette maps to per-vertex colour, a material id, or the field's
  own colour channel.
- What tolerance the round trip is held to, and whether the test is a distance
  bound or a volume-difference bound.
- Whether this belongs with `add-mesh-layers`, which already deals in
  representation boundaries.

## Impact

`voxel-engine` gains the direct conversion. `scene-model` gains whatever object
receives it. `c-abi` and `python-bindings` gain the surface. Additive: nothing
existing changes behaviour.
