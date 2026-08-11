## Why

Issue #67: baking a region with `clay_item_volume_from_document` and putting it
straight back with `CLAY_OP_REPLACE` — no verb applied — shades like corrugated
metal, with a hard rectangle where the sampled box meets the analytic field.
The reporter's worst radial deviation sat at 0.00052 across an eightfold range
of `cell_size`, which ruled out a sampling-resolution artefact and pointed at
something structural. This is the round trip beneath all four of the
reporter's SDF smoothing and planing tools.

**Root cause, measured.** The surface is not where the defect is. Bisecting
the composed field's zero crossing along 325 rays through the replaced region
finds it EXACT at every cell size tried — the constant worst deviation in the
issue is the raycast's stopping tolerance, which is why it does not move with
the cell. What corrugates is the NORMALS. The hard replace
`min(max(a, −b), b)` holds both fields live at the surface; a fresh bake `b`
ties with the field `a` beneath it at every sample plane (trilinear
interpolation of a convex field touches it from above at the lattice); and a
finite-difference gradient across a min/max branch switch pays |b − a| over
its own epsilon. |b − a| oscillates at the cell wavelength, so the normals
ripple at the cell wavelength: measured 32° of worst tilt at `cell 0.04`, 20°
at 0.02, against 0.5° for the volume's own field evaluated alone. The volume
is fine; the composition corrugates it.

The maintainer-narrowed ask after 0.27.3 is two things: a feathered placement
regardless of what produced the volume, and a document-sourced relax to match
`clay_item_volume_flatten_from`.

## What Changes

- **`clay_volume_params` gains a trailing `feather`** (versioned-descriptor
  pattern; zero and every pre-0.28 struct_size mean the hard replace, byte for
  byte). A feathered volume placed with `CLAY_OP_REPLACE` crossfades: deep
  inside the sampled box the result IS the volume — one field, one gradient,
  worst tilt 1.24° at `cell 0.04` and falling with the cell — outside the box
  the surrounding field continues untouched, and the box edge stops being a
  hard rectangle. The correction is clamped at the volume's band, which keeps
  the declared Lipschitz closed-form (max + band·1.5/feather) and keeps
  per-brick culling band-clamp exact; the compiler widens its cull test by
  that band to hold the contract.
- **`clay_item_volume_relax_from`** — the same document-sourced form flatten
  got in 0.27.0, with the same signature shape, the same sampling descriptor
  and the same region convention. For relax the document-sourced form is
  exactly bake-then-relax fused into one call, and a test holds the equality
  rather than assuming it.
- Internally: one new tape combine mode (`ccombine_replace_feather`, emitted
  by the compiler when a feathered volume is placed with Replace — not a
  public op), and the volume blob header grows from 12 to 13 floats by its
  existing self-describing rule.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `c-abi`: the sampling descriptor carries a feather; a document-sourced relax
  is reachable beside the document-sourced flatten.
- `sdf-kernels`: the combine vocabulary gains the feathered replace and the
  volume blob header its feather field, each by the compatibility rule it
  already had.

## Impact

- **ABI**: one added symbol, one descriptor grown at the tail. Additive —
  version moves to 0.28.0, unreleased.
- **Code**: `bindings/c/clay.h`, `bindings/c/clay_c.cpp`,
  `include/clay/field/volume.h`, `src/field/volume.cpp`,
  `include/clay/field/relax.h`, `src/field/relax.cpp`,
  `include/clay/kernel/tape.h`, `include/clay/kernel/exactness.h`,
  `src/scene/tape_build.cpp`.
- **Tests**: the round-trip corrugation converging under a feather (written to
  fail on the old behaviour), feather-zero byte-identity held algebraically,
  the outside-the-box field untouched, relax_from parity with bake-then-relax,
  blob round trip and old-blob compatibility, and per-brick cull bit-identity
  under a feathered replace.
- **Docs**: `docs/07-brushes-and-features.md` (the round trip and its
  feather), `docs/RELEASE.md` version notes.
