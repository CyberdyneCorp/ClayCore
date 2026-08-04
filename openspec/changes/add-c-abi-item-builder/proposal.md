# Proposal: an item builder and versioned structs for the C ABI

## Why

`pyclay` drives the whole engine; the C ABI reaches about a third of the SDF
half. Since the C ABI is how ClaySpace consumes claycore, that gap is the gate
on app integration. This change takes the first half: the authoring surface.

The structural obstacle is `clay_item_desc`, a flat struct passed by pointer.
It cannot grow without breaking every compiled caller, and three of the things
it must carry are variable-length in principle — a deformer chain, a stroke's
points, and a polygon profile's vertices. No fixed struct expresses those.

Doing this first, on its own, de-risks the struct-versioning convention before
the voxel surface is built on top of it.

## What Changes

- **Item builder**: `clay_item_create` / `_destroy`, with setters for
  transform, op, blend, rounding, colour, mirror, a deformer chain,
  repetition, a profile (including polygon vertices), stroke points, and
  transition parameters, then `clay_layer_add_item`. Modifier order is
  preserved, because deformers do not commute.
- **The flat path keeps working**, redefined as sugar over the builder for
  edits that need no variable-length payload.
- **Versioned descriptor structs**: a leading `uint32_t struct_size` the caller
  sets and the library reads only up to, so later fields are additive. The ABI
  hygiene gate enforces it.
- **Complete enumerations**: `clay_prim` gains the 14 backfilled primitives
  plus the two lifts, with values pinned equal to the tape opcodes and a
  static assertion per entry so drift is a compile error rather than silent
  lag. `clay_op` gains the eight extended combine modes and both transitions.

## Capabilities

### Modified Capabilities

- `c-abi`: parity with the Python bindings is stated as the standard; the
  builder pattern, versioned structs and complete enumerations are specified.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `tools/check_c_abi.py`, the C
  smoke test, docs.
- **ABI 0.2.0** — additive; `clay_item_desc` keeps its meaning.
- Voxels, picking, evaluation parity and the parity gate follow in
  `widen-c-abi-voxels`.
- Non-goals: undo/commands, the brick cache, layer instancing — `pyclay` does
  not expose them either. `wrap_around` stays absent on both sides until it
  has a tape opcode.
