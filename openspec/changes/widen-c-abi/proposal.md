# Proposal: widen the C ABI to mirror pyclay

## Why

`pyclay` drives the whole engine. The C ABI reaches about a third of the SDF
half and none of the voxel half, and the C ABI is how ClaySpace consumes
claycore. Concretely, `clay.h` exports 23 functions covering documents, SDF
layers, a 14-primitive item struct, four ops, five blends, point evaluation,
one raycast, meshing and export. Everything else the engine can do is
unreachable from Swift:

- **Voxels entirely** — no grid, palette, brush, sculpting verb, greedy mesh,
  rasterize, or voxel picking. The app's whole voxel-sculpting mode has no API.
- **Half the primitives** — the enum stops at `CLAY_PRIM_PYRAMID`; the 14
  backfilled primitives and the two lifts are absent.
- **Every modifier** — deformers, repetition, profiles, strokes, transitions.
- **The eight extended combine modes**, and cubic/circular/chamfer are present
  but the extended ops are not.
- **Picking beyond one raycast** — no surface snap, no selection or layer
  bounds, no voxel cell/face pick, no build-plane pick.
- **Meshing choice** — no mesher selection; marching only.

This is the gate on app integration and therefore on any honest 1.0. Tagging
1.0 against the current header would commit SemVer to a surface that must
change shape immediately.

## The structural problem

`clay_item_desc` is a flat struct passed by pointer. It cannot grow without
breaking every compiled caller, and three of the things it must carry are
variable-length in principle: a deformer chain, a stroke's points, and a
polygon profile's vertices. No fixed struct expresses those.

So the item path becomes a **builder handle**: create an item, apply modifiers
to it in the order pyclay chains them, then add it to a layer. That mirrors
`clay.Sphere(r=1).twist(1.2).repeat_radial(6)` directly, extends without ABI
breaks, and keeps variable-length payloads out of any struct.

The existing flat `clay_add_item` stays, redefined as sugar over the builder
for the simple case, so current callers keep working.

## What Changes

- **Descriptor structs become versioned.** Every new descriptor carries a
  leading `uint32_t struct_size` that the caller sets and the library reads
  only up to. New fields can then be appended without a major bump, and the
  ABI gate enforces the convention.
- **Item builder**: `clay_item_create` / `_destroy`, with setters for
  transform, op, blend, rounding, colour, mirror, deformer chain, repetition,
  profile, stroke points, and transition, then `clay_layer_add_item`.
- **Full primitive set.** `clay_prim` gains the 14 backfilled primitives plus
  `EXTRUDE` and `REVOLVE`; the enum values SHALL equal the tape opcodes so
  there is no translation table to drift out of sync with `PrimType`.
- **Full op and blend sets**, including the eight extended combine modes and
  both transition morphs.
- **Voxels**: opaque `clay_voxel_grid`, with palette, single/batch/brush edits,
  the falloff `clay_brush_params`, the four sculpting verbs, fills, mirrored
  edits, flood select, greedy meshing, `rasterize`, step-field sampling, and
  voxel/build-plane picking. Both ownership modes pyclay has are mirrored:
  a standalone grid the caller destroys, and a document-attached layer whose
  handle is **borrowed** and must not be destroyed.
- **Picking**: surface snap, layer and selection bounds, and raycast that
  reports the layer and node it hit.
- **Evaluation**: gradients, colours, batch raycast, and `safe_step_scale`.
- **Meshing**: mesher selection (marching, nets, dual contouring behind its
  experimental flag) via a versioned params struct.

## Capabilities

### Modified Capabilities

- `c-abi`: the surface requirement is restated in terms of parity with the
  Python bindings, the builder pattern and versioned structs are specified,
  and handle ownership is made explicit.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `tools/check_c_abi.py`, the C
  smoke test, the Swift smoke, docs.
- **ABI 0.2.0.** Additive for existing symbols; `clay_item_desc` keeps its
  meaning. Pre-1.0, so the minor bump is the right signal.
- Non-goals, because `pyclay` does not expose them either: undo/commands, the
  brick cache, and layer instancing. `wrap_around` stays absent on both sides
  until it has a tape opcode.
