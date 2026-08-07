# Proposal: magnify and pinch

## Why

Two more of ZBrush's standard brushes, and they are the same brush: Magnify
pushes the surface away from the cursor, Pinch pulls it toward it. Maxon's own
page says so — "Magnify: pushes vertices away from cursor; inverse of Pinch".

Pinch is what gives a hard edge its crease and a wrinkle its sharpness; magnify
is what swells a feature without moving it. Neither exists here for SDF layers,
and the voxel side has only pinch.

## The row was named `add-magnify-blob`, and Blob does not belong in it

The roadmap paired them because both were leftovers. They are not the same size.

**Magnify and pinch are one deformer.** A radial scale about a centre with
finite support: sample nearer the centre and the shape reads as expanded,
sample further and it reads as gathered. One signed strength covers both
directions, so implementing "magnify" and then "pinch" separately would be
building the same thing twice with the sign flipped.

**Blob needs a noise source, which does not exist.** Its character is an
*irregular* surface response — that is the whole point of the brush, and the
reason it looks organic rather than machined. The nearest thing here is the
`displace` deformer, whose sine is regular by construction: it would give a
corrugation, evenly spaced, which is what Blob specifically is not. Adding
noise is a real decision — which noise, whether it lives in the kernel dialect
so all four backends agree bit-for-bit, how a seed reaches the tape — and it
belongs to a row of its own rather than being smuggled in behind a brush.

So this row delivers magnify and pinch. Blob is recorded as blocked on
`add-noise-field`.

## What Changes

- **`cdeform_magnify`**: a radial scale about a centre, with finite support and
  an easing curve, as `grab` and `pose` already have. A positive strength
  magnifies, a negative one pinches.
- **Exactness**: a radial scale stretches space, so it is not distance
  preserving and the tape's Lipschitz has to carry it — the same treatment
  `cfi_grab` and `cfi_pose` get. The stretch is largest where the falloff is
  steepest, so the easing curve's slope enters it.
- **`sculpt_magnify`** for voxels, as the inverse of `sculpt_pinch`, so the two
  representations agree on what the verb means.
- **The C ABI, Python bindings**, tests and an example.

## What this change does not do

- **No Blob.** Named above, and blocked on a noise source.
- **No surface-normal magnify.** ZBrush's is radial about the cursor, and so is
  this. Pushing along the local normal instead is `inflate`, a different verb
  the voxel side already has.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `voxel-engine`, `c-abi`, `python-bindings`.

## Impact

- `include/clay/kernel/deform.h`, `tape.h` and `exactness.h`; `scene/types.h`
  for the factory; `voxel/grid.h` for the voxel verb; the C ABI, the Python
  bindings, tests, docs, an example.
