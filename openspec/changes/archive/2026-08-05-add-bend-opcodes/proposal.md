# Proposal: give bend_linear and bend_radial tape opcodes

## Why

`cbend_linear_point` and `cbend_radial_point` are named in the `sdf-kernels`
deformer requirement, implemented in `kernel/deform.h`, and carry exactness
helpers — `cfi_bend_linear` and `cfi_bend_radial` — that have never had a
caller. Like `wrap_around` and `elongate` before them, they exist and no
document can use them. They are the last two.

They are worth having because they are the *ramped* deformers: both displace by
an amount that eases from nothing to full across a region, which is how a
sculptor tilts the top of a form or slumps its rim without affecting the rest.
Twist and bend rotate about an origin; these translate along a gradient.

## The parameter problem, and why the format does not change

`bend_radial` takes `r0`, `r1`, `dz` — three floats, which fit the deformer
record's existing slots.

`bend_linear` takes two segment endpoints and a displacement vector: nine
floats, against four slots. Rather than version the document format, the
encoding exploits something already true of it — **the deformer's type is
written before its parameters**, so a reader can dispatch on it. Existing files
contain only the existing types and are read exactly as before; a deformer that
needs more floats writes more, and only new files contain one. No version bump,
no gated read, no plumbing.

`scene::Deformer` gains a small fixed extension used only by the wide types, and
the tape record widens to match. The tape is rebuilt from the document on every
compile, so its width is free to change.

## What Changes

- **`cdeform_bend_linear` and `cdeform_bend_radial`** join the deformer
  opcodes, both honouring an easing curve like the taper does.
- **Type-dispatched deformer serialization**: the reader takes its float count
  from the type it just read, so the format stays backward-compatible without a
  version bump.
- **Exactness**: the unused `cfi_bend_linear` and `cfi_bend_radial` become the
  callers' Lipschitz source — the ramp's slope is the displacement over the
  span it ramps across, which is exactly what both helpers already compute.
- **Bounds**: both displace by at most the full displacement, so the local
  bound dilates by it.
- Reachable from both bindings, checked on every backend by parity scenes.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: both ramped deformers reach the tape.
- `python-bindings` and `c-abi`: both join the surface.

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build,commands}.cpp`, both bindings, tests, parity corpus, docs.
- ABI 0.8.0 — additive.
- The `.clayspace` format is unchanged for existing content and gains no
  version: old files read identically, new files carry the new types.
