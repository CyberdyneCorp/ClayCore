# Proposal: sampled fields

## Why

Three Phase 2 rows need the same thing and none of them can have it: there is
no way to build a field from sampled data. `sample_step_field` returns
±voxel_size/2 — a bound, not a distance — and nothing in the tree does a
distance transform. So `add-mesh-to-field-import` has nowhere to put its
result and `add-sdf-relax` has no route through voxels.

## A correction to the Phase 2 plan

The plan called this a tape problem: *"the tape is recompiled on every edit, so
a 256³ fp16 volume in the blob means re-uploading 32 MB per brushstroke"*, and
concluded that volumes must live outside the tape behind a resource handle —
an architecture change touching four backends.

**That reasoning assumed dense storage, and it is wrong.** A signed distance
field only needs samples *near the surface*: a narrow band is O(n²), not O(n³).
At 256³ that is ~1.6 MB rather than 33 MB, and at the 64–128³ an imported prop
actually wants, 0.1–0.4 MB — the same order as the 4096-point stroke chain the
blob already carries without complaint.

The second half of the finding was also incomplete: the blob is uploaded **per
eval call**, not per edit — `upload_tape` copies it and releases it every
dispatch. So a resource cache would be a worthwhile optimisation for the whole
blob one day, and it is not something sampled fields specifically require.

The consequence is that this change is much smaller than the plan predicted: a
sparse narrow band rides in the existing blob, every backend gets it from the
shared kernel dialect, and no resource mechanism is needed. A resource cache is
recorded as the follow-up for when volume size actually bites, rather than
being paid for up front against an estimate that was 20× too pessimistic.

## What Changes

- **`FieldVolume`**: a sparse narrow-band signed distance field on a brick
  lattice, built from any callable that answers "what is the distance here".
  Bricks outside the band store no samples — only whether they are wholly
  inside or wholly outside, which is what makes the band sparse.
- **A `Volume` primitive and a `ctape_volume` opcode**, trilinearly
  interpolating within a brick. Bricks carry a one-cell halo so interpolation
  never needs a neighbouring brick, which keeps the lookup O(1).
- **A dense brick index** over the volume's own brick-space bounds, so finding
  the brick for a point is an array read rather than a search. A brick with no
  samples carries a signed lower bound instead of an offset: its gap, in
  bricks, to the nearest brick that does have samples.
- **Exactness declared**: interpolation of samples is not an exact distance,
  and where there are no samples the value is a lower bound rather than a
  distance. Both are declared so the raymarcher stays correct.
- **Serialization** as its own document chunk, so a volume survives a save.

## What building it turned up

Three things the design above got wrong, found by rendering it rather than by
probing values, and fixed here:

- **A flat bound is useless even when it is true.** Every sample-free brick
  first reported the band width, which is correct and stops a marcher dead: the
  steps never grow, so crossing the empty majority of a region exhausts the
  iteration budget. Hence the per-brick gap above.
- **The distance to the sampled box reads as a surface.** It falls to zero on
  the box face, and a sphere tracer treats zero as a hit, so every ray stopped
  on an invisible shell where the sampling stopped. It is now folded together
  with the field at the projected point, which Pythagoras makes exact for a
  projection onto a convex set.
- **The interpolant can be steeper than what it samples.** Trilinear
  interpolation of a 1-Lipschitz field reaches sqrt(3); declaring 1 would let
  the marcher take a full step through a region where the field moves faster.

The first two were invisible to the unit tests, which probed `eval` at points
and never marched a ray. They are covered by tests that march now.

## What this change does not do

- **No resource cache.** Named above, and deliberately deferred.
- **No fp16 packing.** The blob is a float array; halving the volume is
  additive and worth doing when size bites.
- **No adaptive band width.** One width for the whole volume.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `scene-model`, `python-bindings`.

## Impact

- New `include/clay/field/volume.h` + `src/field/volume.cpp`, kernel and tape
  changes, `src/scene/tape_build.cpp`, `src/io/clayspace.cpp`, the Python
  bindings, the parity corpus, tests, docs, an example.
- The C ABI follows in the row that needs it; nothing here is reachable from an
  app until mesh import lands.
