## Why

Issue #609. A mask gates a voxel edit by scaling the dither's weight —
`weight *= 1 - mask` — which is exact at the ends and probabilistic everywhere
between. A cell at mask 0.5 is written half the time, and a mask painted with a
falloff is mostly "in between", so a freeze leaks through its own skirt.

A host cannot fix this from outside. `MaskField` exposes per-point `sample` and
`sample_many` and no bulk cell read, so binarising a mask per stroke means
sampling every footprint cell from the host and rebuilding a second mask field
per dab.

Measured on a slab with a six-cell mask skirt, one solid stroke: 25 cells at
mask >= 0.5 erased anyway, and masked work at 50.8% of unmasked against a
reporting host's bar of a quarter.

## What Changes

- Add `mask_threshold` to the voxel brush parameters and to `clay_brush_params`.
- At zero — the default, and what every existing caller passes — the mask keeps
  scaling the weight exactly as it does today. Bit-for-bit unchanged.
- Above zero the mask is read as a STENCIL rather than a dimmer: a cell whose
  mask is at or above the threshold is refused outright, and a cell below it is
  written as though unmasked.
- Cover the boundary, the default's exactness, and the refusal in regressions.

## Impact

Voxel sculpt weighting and one appended C ABI field. No document payload, no
kernel opcode, no struct re-layout: `clay_brush_params` grows under the
`struct_size` pattern, so a caller compiled against the older struct passes the
older size and gets the behaviour it has now.
