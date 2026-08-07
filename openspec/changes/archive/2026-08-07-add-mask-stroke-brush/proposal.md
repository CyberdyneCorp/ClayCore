# Proposal: the mask brush

## Why

The mask field landed with `add-mask-field` and is the right shape: a sparse
[0,1] scalar on a world-unit lattice, so a resolution change or a move between
the SDF and voxel representations cannot misalign it. What it does not yet have
is the two things that make it a *brush* rather than a data structure.

**Painting a mask is still one stamp per call.** `brush/stroke.h` has exactly
two consumers — `apply_to_grid` (stamps → voxels) and `stamps_to_nodes` (stamps
→ edit-list nodes). There is no mask consumer, so masking along a drag means the
caller re-implements arc-length spacing, pressure response, taper, steady stroke
and jitter, all of which `resolve_stroke` already produces. The same argument the
cut tool and snakehook were built on: leaving the conversion to each caller means
each one answers it differently, and the answer here is not obvious — a `Stamp`
carries a world radius while `MaskField::paint` takes a footprint sized in *mask
cells*.

**And a mask does not actually freeze everything.** It gates voxel edits, every
voxel verb, voxel repair, and SDF edits at the point they are authored. It does
not gate `field::relax` or `field::flatten`, which take a sphere region and
nothing else. So on an SDF layer, "freeze this and sculpt around it" is false for
exactly the two verbs added most recently — and would be false for every field
verb added after them, since there is no convention for them to follow.

## What Changes

- **`brush::apply_to_mask`**: the third stroke consumer. Stamps in, mask painted,
  the same signature shape as `apply_to_grid`. It owns the world-radius →
  mask-cell conversion and states what accumulation means for a field whose
  paint lerps toward a target rather than adding to it.
- **Masks gate the field verbs.** `RelaxSettings` and `FlattenSettings` take an
  optional mask, and the per-sample weight becomes `w * (1 - mask)`. Sampled at
  the world position of the sample being written, which is free — world
  addressing is exactly what makes it free.
- **`MaskField::fill` and `MaskField::invert_within`.** `invert()` flips only the
  chunks that have been touched. That is correct for an unbounded sparse lattice
  and is documented, but it means the most common masking gesture there is —
  mask a limb, invert, sculpt everything else — does not work: the untouched
  chunks stay unmasked and the boundary lands on chunk edges rather than on the
  painted region. A bounded invert takes the finite region from the caller, who
  always has one (a grid's bounds, an item's bounds).
- **The C ABI, Python bindings**, tests and the mask example.

## What this change does not do

- **No mask undo.** Mask edits are not in the command vocabulary, so `paint`,
  `invert`, `clear`, `expand`, `contract` and `smooth` escape undo entirely.
  Real, and the same defect voxel edits have — it wants one answer covering both
  representations rather than two, and that is a change of its own.
- **No mask generators** — by cavity, by ambient occlusion, from a flood select,
  from a colour. Worth having and independent of everything here.
- **No alpha-driven mask stamps.** Blocked on a texture pipeline, like the
  relief brush that wants the same alphas.
- **`invert()` is not changed.** The bounded and unbounded forms answer different
  questions, and quietly redefining the old one would break a caller who had
  already worked around it.

## Capabilities

### Modified Capabilities

- `voxel-engine`, `brush-engine`, `sdf-kernels`, `c-abi`, `python-bindings`.

## Impact

- `include/clay/voxel/mask.h`, `src/voxel/mask.cpp` — bounded fill and invert.
- `include/clay/brush/stroke.h`, `src/brush/stroke.cpp` — the third consumer.
- `include/clay/field/relax.h`, `flatten.h` and their sources — the mask gate.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `bindings/python/pyclay_module.cpp`.
- `tests/unit/test_mask.cpp`, `test_c_mask.cpp`, `test_stroke_engine.cpp`,
  `test_relax.cpp`, `test_flatten.cpp`; `examples/11_masks.py`; docs.
