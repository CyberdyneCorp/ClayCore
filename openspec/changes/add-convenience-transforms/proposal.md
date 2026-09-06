# Proposal: three placements a host cannot write itself correctly

## Why

Every sculpting host has the same three menu items — drop the subtool on the
floor, centre it, send it back to the origin — and the roadmap files them as
"single ABI calls" (Phase 4, `add-convenience-transforms`). The row reads like
sugar. It is not, and reading the header is what shows why.

The obvious host-side implementation is read the placement, compute a
translation from `clay_layer_bounds`, write the placement back. Both halves of
that are traps this ABI already documented and neither is discoverable from the
call the host would reach for first:

- **`clay_document_layer_transform` REFUSES a squashed layer.** The
  single-factor reader returns `CLAY_ERROR_INVALID_ARGUMENT` for a layer
  carrying three different factors, "exactly as `clay_layer_node_transform`
  refuses a squashed node". So the read-modify-write cannot even begin on a
  layer a ZBrush-style gizmo squashed, which since 0.74.0 is a layer any host
  with the per-axis manipulator can produce.
- **`clay_document_set_layer_transform` CLEARS the per-axis scale.** Its own
  header says so: "SETS THE WHOLE PLACEMENT, so it clears any per-axis scale
  the layer carried". A host that gets past the first trap by reading the
  uniform factor from somewhere else and writing through this setter silently
  unsquashes the subtool. The artist snaps to the floor and the model changes
  shape.

The correct implementation is the per-axis pair — read
`clay_document_layer_transform_nonuniform`, write
`clay_document_set_layer_transform_nonuniform` with the three factors and the
rotation carried through untouched — which is four calls, one refusal to know
about and one clearing rule to know about, for a menu item. That is what these
three entry points are: the pair, written once, on the inside.

## What Changes

Three calls, layer-level, in the shape `clay_document_set_layer_transform` and
`clay_layer_bounds` already have:

```c
clay_result clay_layer_snap_to_ground(clay_document* doc, clay_layer_id layer, float ground_y);
clay_result clay_layer_centre_bounds(clay_document* doc, clay_layer_id layer);
clay_result clay_layer_zero_to_origin(clay_document* doc, clay_layer_id layer);
```

- **`clay_layer_snap_to_ground`** translates the layer so the low face of its
  content's world-space box sits at `ground_y`. Y is the only axis it names.
- **`clay_layer_centre_bounds`** translates the layer so the CENTRE of that box
  sits at the world origin. Named for the box because that is what it reads —
  the roadmap spells the row "centre-mass" and the engine has no density; see
  `design.md`, which also records what the occupancy-weighted reading would have
  cost and why the decided signature cannot express it.
- **`clay_layer_zero_to_origin`** sets the placement's TRANSLATION to zero,
  leaving the rotation and both scales alone. It reads no bounds at all, so it
  is the only one of the three an empty layer can take.

All three write the position component of the placement and nothing else. Each
is ONE `SetLayerTransformCmd` — the command that already carries the transform
and the per-axis scale together, for the reason its comment gives — so each is
one undo step and one invalidation, and no new command, opcode or format field
appears anywhere.

Two refusals are the interesting part of the surface, both in `design.md`:
a layer holding no material, and a layer carrying a RADIAL symmetry, whose
copies `clay_layer_bounds` does not cover.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `c-abi`: three convenience placements beside the existing layer placement
  calls, with their refusal set and what they do not promise.
- `scene-model`: a convenience placement is the layer placement command with a
  computed position — one command, one undo step, one invalidation — and it
  reads the tight content bound rather than the influence bound.
- `python-bindings`: the same three reachable from `pyclay`, crossing to C by
  the `Layer` prefix rule the parity gate already applies.

## Impact

- **ABI**: three added symbols, no struct grown, no signature changed. Additive
  — the library and ABI minor move 0.85.0 -> 0.86.0 in the implementing PR
  (`CMakeLists.txt`, `bindings/c/clay.h`, `pyproject.toml`, which must agree).
- **Code**: `bindings/c/clay.h`, `bindings/c/clay_c.cpp`,
  `bindings/python/pyclay_module.cpp`, plus four small functions in
  `include/clay/scene/placement.h` and `src/scene/placement.cpp`. The bound and
  the command both already exist and neither moves — but pyclay does not go
  through the C ABI, so the policy about WHICH placement fields a computed
  placement writes has to live below both bindings or be written twice. See
  "What building it found" in `design.md`.
- **Format**: none. `kSceneMinor` does not move — a convenience placement saves
  as the placement it produced, which every existing minor already stores.
- **Tests**: the squashed-layer round trip (the defect the change exists for),
  each rule against a hand-computed box, one undo step each, the empty and
  radial refusals, a hidden layer accepted, an instance not severed, and the
  three representations (SDF, voxel, mesh) taking the same rule.
- **Docs**: `docs/05-*` library reference entry for the three; `docs/RELEASE.md`
  at release time.

## Non-goals

- **Item-level variants.** Deliberately not taken; the reason and the reason it
  costs nothing to add later are in `design.md`.
- **Widening `clay_layer_bounds` to cover radial copies.** Real, found while
  writing this, and its own change: it alters what a camera-framing query
  answers for every existing host. This change REFUSES the case rather than
  answering it wrongly.
- **A whole-document snap.** N layers is N calls, bracketed by
  `clay_document_begin_undo_group` when a host wants one step. A document-level
  call would have to decide what "the document's ground" means for hidden and
  ghosted layers, and the host already knows.
