# Expose the mixed-depth export

## Why

`finish-regional-multires` built the mixed-depth export in C++ —
`MultiresSurface::mixed_mesh_at_level` and `MultiresSurface::build_mixed_block`,
with `MultiresMixedStatus` naming a refusal — and gated it there: 0 open edges
on a closed 144-patch torus with a 2x2 region at level 3, where the per-patch
loop `clay.h` tells a host to write leaves 72 / 168 / 264 at display levels
1 / 2 / 3. Nothing in `clay.h`, pyclay or the Swift surface reaches it, so a
host on any binding still assembles per patch and still meets those counts.

That change carried the entry point as its tasks 5.7 and 6.1 and did not build
it, for a reason recorded there: no host can meet a mixed-depth seam. Neither
host checked on 2026-09-23 (ClaySpace, ClaySpaceDesktop) calls
`clay_multires_refine_patches_to_level`, so neither ever holds a regional
hierarchy. ClaySpaceDesktop does bake one — its mesh conversion calls
`clay_multires_copy_level_mesh` at the display level — but on a UNIFORM
hierarchy, where that call is already the level's own watertight mesh. Adding
ABI surface no caller can exercise is maintenance with no user, so the two tasks
were moved here rather than built, and `finish-regional-multires` archived with
them named.

This change is the place that work waits. It is not scheduled; it starts when a
host refines a region through the ABI and bakes the result — the first host to
call `clay_multires_refine_patches_to_level` and then `clay_multires_copy_level_mesh`
meets the open edges below.

## What Changes

- A C entry point for the whole-surface mixed export and a mirrored per-patch
  pair, following `clay_multires_block_info_get` and `clay_multires_copy_block`
  in shape, but NOT repeating `clay_multires_copy_block`'s answer of success
  with an empty block for a non-resident patch.
- pyclay and Swift mirrors, so `check_binding_parity.py` holds.
- The ABI minor moves in the PR that adds the entry point.

## Capabilities

### Modified Capabilities

- `c-abi`: the mixed-depth export becomes reachable across the boundary.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `bindings/python/pyclay_module.cpp`,
  the Swift surface, `tools/check_c_abi.py`'s struct mirror.
- The three version lines.
- No engine change: the C++ export and its gates are already on main.
