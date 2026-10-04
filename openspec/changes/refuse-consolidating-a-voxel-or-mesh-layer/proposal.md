## Why
Consolidating a voxel or a mesh layer answered `CLAY_ERROR_INVALID_ARGUMENT` with the detail meant for an EMPTY SDF layer (#659). Probed at v0.120.1 on a freshly added voxel layer:

```
clay_layer_consolidate -> CLAY_ERROR_INVALID_ARGUMENT
"nothing to consolidate: the layer is empty, unbounded, or the region contains no surface"
```

ClaySpaceDesktop showed that detail on `layer/optimize` over a grid layer, and artists read it as "already optimised". The real answer is "this action only applies to SDF layers". A host could not tell the two apart without asking `clay_document_layer_info` first, so every host had to repeat the representation check to give the right reason.

The entry points only checked for a missing or protected layer. `clay_layer_consolidation_cost` and `clay_layer_consolidate_cancellable` (and `clay_layer_consolidate` through it) then baked, and the bake returns false for a layer with no SDF content (`src/scene/consolidate.cpp`). `clay_layer_consolidate_region` failed the same way with "nothing to merge". `clay_layer_plan_region_merge` succeeded with a plan that absorbs nothing, for a merge the commit would then refuse.

## What Changes
- `clay_layer_consolidation_cost`, `clay_layer_consolidate`, `clay_layer_consolidate_cancellable`, `clay_layer_plan_region_merge` and `clay_layer_consolidate_region` refuse a voxel or mesh layer with `CLAY_ERROR_UNSUPPORTED`. The check runs right after the not-found check, before the protection check and before the params are read or anything is sampled. `clay_last_error` names the representation: "consolidation applies to SDF layers: layer N is a voxel layer" (or "a mesh layer"). The wording follows `clay_document_instance_layer`'s refusal.
- An empty SDF layer still answers `CLAY_ERROR_INVALID_ARGUMENT` "nothing to consolidate" (or "nothing to merge"). `clay_layer_plan_region_merge` still plans zero absorbed roots for it and succeeds.
- `clay_layer_consolidation_advice` is unchanged. It still succeeds, advises nothing and zeroes on a voxel or mesh layer. Its header has documented that behaviour since 0.86.0, following `clay_layer_warp_cost_get`'s rule for a stack of mixed kinds, and a test asserts it. "Should I bake this?" has an answer for a grid; "bake this" does not. `clay_layer_consolidation_state` also stays a plain query that answers 0.
- The `clay.h` comments on all five refusing calls, the advice note, and `docs/05` state the new error contract.

## What building it found
- The triage plan also put `clay_layer_consolidation_advice` behind the refusal. That would have broken a documented contract and an existing test ("the advice refuses before it samples, and a mixed stack is an answer"). So it was left alone, and the new test pins its answer for a voxel and a mesh layer.
- Representation goes before protection on purpose. A locked grid would otherwise answer "layer is protected", which sends the artist to unlock a layer that could never be consolidated.

## Impact
This is a behaviour change on an error path only. A call that used to fail with `CLAY_ERROR_INVALID_ARGUMENT` on a voxel or mesh layer now fails with `CLAY_ERROR_UNSUPPORTED`. `clay_layer_plan_region_merge` on such a layer used to succeed with an empty plan and now fails. No signature, struct or enumerator changes, and the ABI version line does not move. No format change.
