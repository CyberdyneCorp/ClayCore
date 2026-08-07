# Proposal: mask extrude

## Why

Mask a region of a surface and pull that patch off as a solid of a chosen
thickness. ZBrush calls it Extract, 3DCoat reaches it through Extrude from a
frozen area, and it is how armour plates, panels, straps, pockets and shells get
made — the single most-used thing a mask is *for*, after freezing.

Nothing here does it. The mask can be painted and it can freeze an edit; it
cannot produce geometry.

## The blocker was one conversion, not a new mechanism

Everything else already exists. `op_shell_union`'s operand is literally
`abs(d) - t`, the shell of a field. `FieldVolume` is a narrow-band sampled field
that rides in the tape's blob and evaluates identically on all four backends.
`field::flatten` established the pattern for a verb that cannot rewrite samples
in place: sample a fresh volume over a region and hand it back.

What was missing is that a `MaskField` is a **[0,1] scalar on a lattice, not a
distance field**. Composing it into a field expression directly would put a
near-vertical step in the result, and the Lipschitz bound the raymarcher depends
on would be a fiction. So this change adds one conversion —

    mask_to_field: signed distance to the boundary of { mask >= threshold }

computed by a Euclidean distance transform over the mask's painted region and
stored as an ordinary `FieldVolume`. It comes out 1-Lipschitz, tape-expressible
and blob-carried, and after that the extract is ordinary op composition.

**The mask IS the region.** Relax and flatten both need a `region_radius`
because they have no other way to know where to act. Extract does not: the
painted region bounds itself, which is why this needs no region parameter and
why it samples a smaller volume than either of them.

## What Changes

- **`mask_to_field`**: mask → narrow-band signed distance to the masked region's
  boundary. Public, because a host wants to preview that border, and because a
  mask-referenced pose region will want the same thing.
- **`mask_extrude` on a field**: `smooth_max(shell(source), mask_distance, r)`,
  sampled into a new volume over the mask's padded bounds. Outward, inward or
  centred; the rounded intersection is the soft rim.
- **`mask_extrude` on a voxel grid**: the same verb in cell space, no
  resampling — copy the masked surface cells and dilate outward. The two
  representations must agree, the discipline `add-magnify-pinch` set.
- **The C ABI, Python bindings**, tests and an example.

## What this change does not do

- **No mesh-level extract.** What comes back is a field or a grid; meshing it is
  `marching`, `surface_nets` or `dual_contouring`, all of which already work.
  There is nothing to add.
- **No parametric link to the source.** The extract is a snapshot, for the reason
  flatten's output is: a live one needs a tape op that references another layer,
  which does not exist and is a much larger question than this row.
- **No rim profile.** `border_round` covers the common case; a user-defined
  cross-section on the rim is `add-blend-profile-curves`.
- **It does not consume the mask.** ZBrush leaves it, extracting twice at two
  thicknesses is a real thing to want, and a caller who wants it gone has
  `clear()`.

## Capabilities

### Modified Capabilities

- `voxel-engine`, `sdf-kernels`, `c-abi`, `python-bindings`.

## Impact

- New `include/clay/field/mask_extrude.h`, `src/field/mask_extrude.cpp`.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `bindings/python/pyclay_module.cpp`.
- New `tests/unit/test_mask_extrude.cpp`, `tests/unit/test_c_mask_extrude.cpp`;
  new `examples/25_mask_extrude.py` and its entry in `CAPABILITY_EXAMPLES`;
  docs and roadmap.
