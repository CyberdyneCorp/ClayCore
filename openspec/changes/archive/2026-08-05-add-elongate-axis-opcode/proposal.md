# Proposal: give elongate_axis a tape opcode

## Why

The last unreachable kernel feature. `celongate_axis_point` is the companion to
`celongate_point`: where the latter is exact but only about the origin, this one
works on any primitive, symmetric or not, at the cost of being a bound inside
the stretched region.

That trade is worth exposing precisely because `elongate` cannot make it. Today
a caller who wants to stretch an asymmetric primitive — a cone, a cut sphere, a
pyramid — gets `elongate`'s origin-derived correction, which the compiler
correctly flags as a bound anyway. `elongate_axis` gives the same stretch with a
map that is honest about what it does: `p - clamp(p, -h, h)`, a pure per-axis
translation of the outside toward the middle, with a flat plateau inside.

## What Changes

- **`cdeform_elongate_axis` joins the deformer opcodes**, taking the three
  half-extents in the record's existing `k`/`a`/`b` slots.
- **No distance correction.** Unlike `elongate`, this map contributes nothing to
  the distance — the flat interior plateau is exactly what makes it a bound
  rather than exact, and adding a correction would be wrong.
- **Exactness**: always a bound inside the stretch region, regardless of the
  primitive, so the tracked field info downgrades unconditionally. The map is
  non-expansive, so the step scale is untouched — the same shape of trade
  `elongate` makes on an asymmetric primitive.
- **Bounds** expand per axis by `h`, as with `elongate`.
- Reachable from both bindings, checked on every backend by a parity scene.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: per-axis elongation reaches the tape.
- `python-bindings` and `c-abi`: it joins the surface.

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build}.cpp`, both bindings, tests, parity corpus, docs.
- ABI 0.9.0 — additive.
- With this landed, every function in the kernel headers is reachable from a
  document.
