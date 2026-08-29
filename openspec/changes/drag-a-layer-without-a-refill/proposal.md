# Proposal: placing a layer rigidly should not re-evaluate its field

## Why

Moving or rotating a whole layer — the Move/Rotate/Scale gizmo on an object —
changes no shape. Layers combine by hard union at the document level (stated
under "Consolidation is one undoable command"), and a layer's transform enters
the compiled tape only as a rigid matrix composed into each item's inverse
(`src/scene/tape_build.cpp:670`), with rounding and blend scaled by
`layer.xform.scale`. So for a transform that is rigid, or rigid plus a uniform
scale, **the layer's surface afterwards is exactly its surface beforehand, moved
by the same matrix**, and there is no cross-layer blend to re-solve.

The engine does not know that. `SetLayerTransformCmd` reaches
`command_influence_bound` as the whole layer's box on both sides of the apply,
`command_frontier` excludes the layer-parameter commands by name
(`bindings/c/clay_c.cpp:2948`), so every seed the layer's bound touches is
dropped and every brick in it re-walks the full tape. A gizmo drag pays that on
every frame it moves.

Measured on this Mac, CPU backend, 5832 bricks of 8³ at 0.05 voxel over a
1000-item blob:

| per drag frame | 100 items | 1000 items |
|---|---:|---:|
| the edit alone | 0.031 ms | 0.133 ms |
| the same refill with no edit at all | 3.7 ms | 17.4 ms |
| layer translate + display | 12.4 ms | **95.7 ms** |
| layer rotate + display | 12.3 ms | **95.7 ms** |
| `clay_mesh_transform` of the finished 120k-vertex surface | 0.32 ms | **0.30 ms** |

The last row is the same result by the only other route, and it includes
allocating a second copy of the mesh. The engine spends 95.7 ms to produce what
0.30 ms of matrix multiply already has.

Against the 4.17 ms interactive frame share, a layer drag is 23x over budget at
1000 items — and there is no device case for it at all, so this has never been
gated on hardware.

## What Changes

Two phases, each verifiable on its own.

**Phase 1 — the engine says when a placement is rigid, and is held to it.**

- A layer transform is classified: RIGID (rotation and translation), SIMILARITY
  (those plus a uniform positive scale), or GENERAL (anything else — today only
  a non-uniform layer scale, which is not expressible through the current entry
  point but is through the document). The classification is reported to the
  host along with the matrix taking the old placement to the new one.
- The guarantee is written down and gated: after a rigid or similarity layer
  transform, the layer's own surface equals its previous surface mapped through
  that matrix, and the distance field equals the previous field composed with
  the inverse, scaled by the uniform factor.
- Invalidation is UNCHANGED in phase 1. The host may act on the report; the
  engine still drops what it dropped before, so nothing can go stale on a host
  that ignores it.

**Phase 2 — a transform gesture, so a drag pays once.**

- `begin` / `update` / `end` for a layer placement. While the gesture is open,
  `update` records the placement and performs NO invalidation. `end` applies one
  `SetLayerTransformCmd` and one region invalidation — the drag costs one
  refill, not one per frame. A gesture abandoned without `end` restores the
  placement it began with, and one left open when the document is edited
  elsewhere is refused.
- So the host can draw the preview, the document becomes evaluable **with one
  layer excluded** and **as one layer alone** — the split the tape compiler
  already has internally for the below/active halves, generalised from "the
  active layer" to "a named layer" and lifted to the ABI on both the brick path
  and the mesh path:
  - bricks: evaluate requests against the document minus the dragged layer, and
    against the dragged layer alone, each once at gesture start;
  - mesh: mesh one SDF layer, which the ABI cannot do today —
    `clay_document_mesh_layer` borrows an imported MESH layer's triangles and is
    a different call.
- The host then draws the two, the dragged one under the gesture matrix. Because
  layers union hard, that is exact for each surface; what a preview does not
  show is the mutual occlusion of the union while the two overlap, which
  resolves on `end`. That limit is stated rather than discovered.

## Capabilities

### New Capabilities
None. Both phases add requirements to existing capabilities.

### Modified Capabilities
- `scene-model`: what a rigid or similarity layer placement guarantees about the
  layer's field and surface, and the classification a placement carries.
- `c-abi`: the placement report, the transform gesture, and evaluating the
  document with a layer excluded or as that layer alone.
- `brick-cache`: a refill may be asked for a named layer, or for everything but
  it, at the same lattice and with the same values as the whole-document form
  where they agree.
- `meshing`: meshing one SDF layer, in world space under that layer's transform.

## Impact

- `include/clay/math/transform.h`, `src/scene/` — the classification, and the
  matrix from one placement to another.
- `src/scene/tape_build.cpp` — `compile_document_part` generalised from the
  active/below split to a named layer, or its complement.
- `bindings/c/clay_c.cpp` — the report, the gesture (an RAII region like
  `GestureRegion`, holding its invalidation until `end`), the two evaluation
  forms, the mesh form.
- `bindings/c/clay.h`, `bindings/python/` — ABI additions, minor version.
- `tests/` — the rigidity gate (surface equality under the matrix), the gesture's
  once-only invalidation, per-layer evaluation agreeing with the whole.
- `tests/device/` — `layer_transform_drag`, per `add-device-transform-cases`.

## Non-goals

**A non-uniform layer scale.** It changes the field's Lipschitz behaviour, as
`cfi_scale_nonuniform` already records for items. It classifies as GENERAL,
takes today's invalidation, and this change says nothing else about it.

**Node-level transforms.** Dragging one item inside a layer genuinely changes
the field where it was and where it goes. Making that region tight is
`bound-an-edit-by-the-node-it-names`; this change is about the case where the
region should be nothing at all.

**Voxel and mesh layers.** A voxel layer's grid is not rigidly re-samplable and
a mesh layer already transforms its own vertices. The classification is reported
for any layer; the field guarantee and the per-layer evaluation are stated for
SDF layers.

**Making the preview show the union.** A preview draws two surfaces that may
interpenetrate. Resolving the union live would mean evaluating the composite,
which is the cost this change exists to avoid.
