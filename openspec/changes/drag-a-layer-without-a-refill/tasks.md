# Tasks

## Phase 1 — the placement is classified, and the engine is held to it

- [x] 1.1 Classify a placement: rigid, similarity (with its factor), general
- [x] 1.2 The delta matrix from the previous placement to the new one, taken before the apply
- [x] 1.3 The report, as a QUERY beside the layer-transform entry points rather than an output on them — which keeps their signatures unchanged AND makes 1.7 true by construction
- [x] 1.4 Gate: an SDF layer meshed, placed rigidly, meshed again — the second mesh equals the first through the matrix, and field values agree exactly at mapped points
- [x] 1.5 Gate: a uniform scale multiplies distances by its factor
- [x] 1.6 Gate: re-placing one layer of a multi-layer document leaves every other layer's field bit-identical
- [x] 1.7 Test: asking for the report changes neither the document, the invalidation, nor a subsequent refill

## Phase 2 — a drag costs one refill

- [x] 2.1 `run_part` generalised from "before `stop`" / "`stop` alone" to "except `layer`" / "`layer` alone" — **already present**: #378 landed `Compiler::Part::Except` and `compile_document_except`, and `Part::Only` is "layer alone". Audited rather than rebuilt
- [x] 2.2 A scoped refill stores NO seed, which is stronger than keying one by scope and is **already true**: `eval_requests_in_chunks` gates only the `Whole` half, as its own comment says. Gated by 2.4 rather than rebuilt
- [x] 2.3 Test: the pointwise minimum of the two scoped results equals the whole document's, on a three-layer document
- [x] 2.4 Test: a scoped refill followed by an unscoped one with no edit between gives cold unscoped values
- [x] 2.5 The brick evaluation calls take a layer scope
- [x] 2.6 Meshing one named SDF layer, in world space under its transform; refused for a non-SDF layer; a hidden layer meshes by name
- [x] 2.7 The placement gesture: open, update, close, cancel — modelled on `GestureRegion`, with the region known up front
- [x] 2.8 `edit_guard` refuses every other edit while a gesture is open
- [x] 2.9 Test: sixty updates and a close record one command, perform one invalidation, and dirty what one placement would
- [x] 2.10 Test: the document does not move until the gesture closes; a closed gesture undoes in one step; a cancelled one leaves the opening placement
- [x] 2.11 Test: a gesture on a locked or ghosted layer is refused at the open
- [x] 2.12 Python bindings for the report, the gesture and the scoped calls
- [x] 2.13 An example: drag a layer, drawing the two scopes, committing on drop
- [x] 2.14 Benchmark: a 60-frame layer drag, one refill against sixty
- [x] 2.15 `docs/09-brush-latency-and-coverage.md` — the layer-drag row, from the device figures `add-device-transform-cases` produces

## What the plan got wrong, found by measuring

- [x] 3.1 **A uniform scale is not a similarity of a blending layer.** The layer
      scale multiplies an item's rounding and not its blend radius: two boxes
      blended at k=0.12 and scaled by 2 came out at a ratio of 1.289 where a
      similarity says 2. The classification refuses it instead; fixing the
      inconsistency itself would change what existing documents evaluate to and
      is left as its own decision.
- [x] 3.2 **The field equality is exact in exact arithmetic, not
      unconditionally.** The tape folds `layer.xform * item.xform` into one
      inverse matrix, so a placement re-rounds every item. The bit-exact gate
      uses positions that survive that composition; the realistic gates assert a
      few ulp (4.17e-07 worst, under a rotation).
- [x] 3.3 `mesh_params.resolution` divides each mesh's OWN bounds, so comparing
      a one-layer mesh with a whole-document mesh at the same `resolution`
      compares two densities. The gate pins `voxel_size` instead.
