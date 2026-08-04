# Proposal: voxels, picking and evaluation parity for the C ABI

## Why

The second half of closing the gap between the C ABI and `pyclay`. With the
item builder landed, SDF authoring is reachable from Swift; this change makes
the rest reachable: the entire voxel engine, picking beyond a single raycast,
the remaining evaluation entry points, and mesher selection.

ClaySpace's voxel-sculpting mode currently has no API at all — not a reduced
one, none — so this is the change that actually unblocks that half of the app.

It ends with a gate, because the ABI fell behind the bindings silently once
already and will do so again without one.

## What Changes

- **Voxel grids** behind an opaque handle: palette management, single and
  batch edits, cube/sphere brushes with falloff and strength, the four
  sculpting verbs, fills, mirrored edits, flood select, occupancy and bounds
  queries, greedy meshing, SDF rasterization, and step-field sampling.
- **Explicit ownership**: a standalone grid is owned by the caller and
  destroyed explicitly; a grid obtained as a document layer is borrowed and
  must not be destroyed. Destroying a borrowed handle returns an error rather
  than corrupting the document — the classic C ABI footgun, and Swift will not
  protect anyone from it.
- **Picking**: surface snapping, layer bounds, selection bounds, voxel
  cell/face picking, build-plane picking, and raycast that attributes the hit
  to a layer and node.
- **Evaluation**: gradients, field colours, batch raycast, safe step scale.
- **Meshing**: mesher selection (marching, nets, dual contouring behind its
  experimental flag) through a versioned params struct.
- **A parity gate** that enumerates the capability surface `pyclay` exposes and
  fails when a C entry point is missing without a recorded exemption, plus an
  ABI-level check that every function `clay.h` declares the library exports.
- **ABI 0.3.0**: 44 entry points, three enumerations and two appended
  `clay_mesh_params` fields are a surface a host has to be able to tell apart
  from 0.2.0's, and below 1.0 consumers compare the minor.
- **A documented batch ceiling** (`CLAY_MAX_BATCH`) on every count crossing the
  boundary: the library builds without exceptions, so an allocation sized from
  a wrong count would terminate the host process rather than return an error.

## Capabilities

### Modified Capabilities

- `c-abi`: voxels, picking and evaluation parity join the surface, with
  ownership rules and an enforced parity gate.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `tools/check_c_abi.py`, a new
  parity gate, the C and Swift smokes, docs.
- Depends on `add-c-abi-item-builder` for the versioned-struct convention.
- Non-goals: undo/commands, the brick cache, layer instancing.
