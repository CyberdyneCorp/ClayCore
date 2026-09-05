## Why

Surface groups name a region of the model. Masks gate what a verb may touch.
The two already interoperate in ONE direction — `clay_groups_fill_from_mask`
takes a painted mask and names it — and the header says why that matters:

> "how 'addressed by group or by mask' becomes one mechanism rather than two"

The reverse does not exist. So a group can reach a brush only through
`CLAY_AUTOMASK_SURFACE_GROUP` — "stay inside the polygroup the brush started
in" — and never through a mask a caller CHOSE.

That is a different operation from the one artists arriving from ZBrush or
Blender expect. "Flatten this whole panel" is a SELECTION followed by a
decision; what the ABI offers is a stroke that happens to begin on the panel.
And every verb that already respects a mask — freeze, invert-and-sculpt-the-
rest, relax, extrude — is unreachable from a group for the same reason.

## What Changes

One call, mirroring `clay_groups_fill_from_mask` in the other direction:

```c
clay_result clay_mask_fill_from_group(clay_mask* mask, const clay_groups* groups,
                                      uint16_t group, float value, uint64_t* out_cells);
```

- **No region argument**, on the same terms as its mirror: the group's own
  extent drives it. A field knows where it is, and a caller-supplied box is how
  two lattices get to disagree about one border.
- **The two cell sizes need not match.** Cells are sampled at the MASK's
  centres, so a fine mask over a coarse group quantises to the group.
- **Zero ERASES.** It is the value that releases a cell's storage everywhere
  else in this API, so a group can un-mask its own region rather than leave
  explicit zeros behind — a mask full of zeros and an empty mask gate
  identically and do not cost the same.
- **`CLAY_NO_GROUP` paints nothing** and is not an error: "not in a group" is
  not a region, and painting the complement of every group is a different
  request a caller can express by inverting.

## What it does not change

**The border is the group's, cell-quantised** — the same border every other
group operation draws, and the cost the surface-group design states up front. A
mask painted this way cannot be finer than the lattice that named the region.
Nothing here moves toward per-face group ids; that is a separate design question
about whether a mesh layer may carry its own, and it is not answered by this.

## Result

Round-tripped on a named half-ball at a 0.1 cell: 10,976 group cells become
10,976 mask cells, and naming them back gives the same 10,976 — agreeing with
`clay_groups_at` at every one of 1,331 probe points rather than merely by count.
Erasing with zero takes a fully painted mask from 21,952 cells to 10,976, which
is storage released rather than zeros written.

## Capabilities

### Modified Capabilities
- `c-abi`: a named region can be turned into a mask, so a group is a selection.

## Impact

- `include/clay/voxel/mask.h`, `src/voxel/mask.cpp` — `MaskField::fill_from_group`.
- `bindings/c/clay.h`, `clay_c.cpp`; `bindings/python/` — `MaskField.fill_from_group`.
- **ABI 0.84.0 -> 0.85.0.** Additive; no existing signature moves.
