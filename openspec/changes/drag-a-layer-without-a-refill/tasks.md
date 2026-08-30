# Tasks

## Phase 1 — the placement is classified, and the engine is held to it

- [ ] 1.1 Classify a placement: rigid, similarity (with its factor), general
- [ ] 1.2 The delta matrix from the previous placement to the new one, taken before the apply
- [ ] 1.3 The optional report on the layer-transform entry points; the existing signatures unchanged
- [ ] 1.4 Gate: an SDF layer meshed, placed rigidly, meshed again — the second mesh equals the first through the matrix, and field values agree exactly at mapped points
- [ ] 1.5 Gate: a uniform scale multiplies distances by its factor
- [ ] 1.6 Gate: re-placing one layer of a multi-layer document leaves every other layer's field bit-identical
- [ ] 1.7 Test: asking for the report changes neither the document, the invalidation, nor a subsequent refill

## Phase 2 — a drag costs one refill

- [ ] 2.1 `run_part` generalised from "before `stop`" / "`stop` alone" to "except `layer`" / "`layer` alone"
- [ ] 2.2 The resume store keys an entry by its scope; a scope it does not hold is a miss
- [ ] 2.3 Test: the pointwise minimum of the two scoped results equals the whole document's, on a three-layer document
- [ ] 2.4 Test: a scoped refill followed by an unscoped one with no edit between gives cold unscoped values
- [ ] 2.5 The brick evaluation calls take a layer scope
- [ ] 2.6 Meshing one named SDF layer, in world space under its transform; refused for a non-SDF layer; a hidden layer meshes by name
- [ ] 2.7 The placement gesture: open, update, close, cancel — modelled on `GestureRegion`, with the region known up front
- [ ] 2.8 `edit_guard` refuses every other edit while a gesture is open
- [ ] 2.9 Test: sixty updates and a close record one command, perform one invalidation, and dirty what one placement would
- [ ] 2.10 Test: the document does not move until the gesture closes; a closed gesture undoes in one step; a cancelled one leaves the opening placement
- [ ] 2.11 Test: a gesture on a locked or ghosted layer is refused at the open
- [ ] 2.12 Python bindings for the report, the gesture and the scoped calls
- [ ] 2.13 An example: drag a layer, drawing the two scopes, committing on drop
- [ ] 2.14 Benchmark: a 60-frame layer drag, one refill against sixty
- [ ] 2.15 `docs/09-brush-latency-and-coverage.md` — the layer-drag row, from the device figures `add-device-transform-cases` produces
