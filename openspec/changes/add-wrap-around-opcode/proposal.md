# Proposal: give wrap_around a tape opcode

## Why

`cwrap_around_point` has been in `kernel/deform.h` since the first release and
no document can carry it. There is no `cdeform_*` opcode, so the tape cannot
express it, so both bindings refuse it — `pyclay`'s `.wrap_around()` exists only
to raise an error naming the reason. It is the one deformer that does not work,
and the last such gap in the SDF vocabulary.

It is also the deformer an app most obviously wants and cannot fake: bending a
flat relief or a line of text around a column is not expressible as a
composition of twist, bend, taper and displace.

The exactness side is already designed and equally unused: `cfi_wrap_around`
sits in `kernel/exactness.h` with no caller and no test. This change makes both
reachable rather than inventing anything.

## What Changes

- **`cdeform_wrap` joins the deformer opcodes**, with `x0`/`x1` in the record's
  existing `k`/`a` slots, so the 6-float deformer record is unchanged and no
  tape layout moves.
- **Bounds**: the wrap sends the flat interval to a full turn about the Z axis,
  so the deformed bound is the disc swept by the content's radial extent —
  `|x|,|y| <= max(|r + ymin|, |r + ymax|)` with `r = (x1 - x0) / 2pi`, `z`
  unchanged. Conservative for items that occupy only part of the interval, which
  is the safe direction.
- **Exactness**: the deformer is a metric breaker like the others, and its
  Lipschitz factor comes from the existing `cfi_wrap_around(info, x0, x1, t)`
  with `t` the content's radial extent — the same convention twist, bend and
  taper already use, where the constant is bounded by the item's own geometry
  rather than by the whole space a ray might traverse.
- **Reachable from both bindings**: `pyclay`'s stub becomes a real modifier and
  the C ABI gains `CLAY_DEFORM_WRAP`, so the parity gate's `Prim.wrap_around`
  exemption is retired rather than reworded.
- **Device parity**: a wrap scene joins the parity corpus, so Metal, OpenCL and
  CUDA are checked against the scalar reference for it like every other opcode.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: the deformer requirement covers wrap_around, and the tape can
  carry it.
- `python-bindings` and `c-abi`: wrap_around joins the surface.

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/bounds.cpp`, `src/scene/tape_build.cpp`, both bindings, tests, the parity corpus, the reference evaluator, docs.
- ABI 0.6.0 — additive: a new enumerator, no layout change.
- Non-goals: the `bend_linear` and `bend_radial` exactness helpers, which are
  in the same unused state but have no kernel implementation behind them; they
  would each need their own deformer, not just an opcode.
