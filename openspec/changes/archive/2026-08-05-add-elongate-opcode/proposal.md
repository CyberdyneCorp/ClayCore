# Proposal: give elongate a tape opcode

## Why

`celongate_point` is in `kernel/xform.h`, is named by the `sdf-kernels`
requirement for that header, appears in docs/01 §2.3, and is listed in §2.7
among the operations that **stay exact** — and no document can use it. Like
`wrap_around` before it, the implementation has been there since the first
release with nothing wired to it.

It matters more than its size suggests. Elongation inserts flat sections into
a shape — stretching a capsule, a box or a torus without distorting its caps —
which is a routine modelling move that otherwise requires authoring a
different primitive. And unlike every deformer that currently reaches the
tape, it costs nothing in step scale: the map is non-expansive, so an
origin-symmetric primitive stays exact through it.

## What Changes

- **`cdeform_elongate` joins the deformer opcodes.** Elongation both warps the
  point and contributes a distance correction, and the tape's chain already
  evaluates the offset at the pre-warp point and then warps — which is exactly
  the order elongation needs. `h` occupies the record's existing `k`/`a`/`b`
  slots, so the 6-float record and the tape layout are unchanged.
- **Exactness is conditional, which is new.** Every deformer so far is a metric
  breaker, so `fold_info` downgrades on a Lipschitz above 1. Elongation is
  1-Lipschitz and *exact for origin-symmetric primitives only*, so exactness
  now has a second reason to drop that is independent of the Lipschitz factor:
  `prim_is_origin_symmetric` decides, and the conservative direction is a
  bound.
- **Bounds** expand per axis by `h`, which is exactly what the flat sections
  insert.
- Reachable from both bindings, checked on every backend by a parity scene.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: elongation reaches the tape, with its exactness condition
  stated.
- `python-bindings` and `c-abi`: elongate joins the surface.

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build}.cpp`, both bindings, tests, parity corpus, docs.
- ABI 0.7.0 — additive.
- Non-goals: `elongate_axis`, which is the asymmetric-primitive variant and a
  bound everywhere inside the stretch region; it follows separately.
