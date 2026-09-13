# c-abi — the geometry revision moves when history replaces the triangles

Delta for `own-the-mesh-invalidation-signal`.

## MODIFIED Requirements

### Requirement: A mesh layer can be rebuilt through the document
The C ABI SHALL expose a rebuild that targets a mesh LAYER: capture, rebuild, validate, replace and record, as one call and one undo step, taking the same versioned parameter descriptor and filling the same versioned report as the pure mesh-to-mesh form.

It SHALL be transactional. Nothing is written until the rebuild has succeeded and validated, so a refusal, a validation failure or a cancellation leaves the layer byte-identical and adds no undo step.

A protected layer SHALL be refused BEFORE the rebuild rather than after it: rebuilding a locked layer for several seconds and then declining to commit is a worse answer than declining immediately.

The ABI SHALL also expose the layer's geometry revision and a revision-checked replacement, so a host that ran the pure rebuild on its own worker thread can commit it without overwriting newer work. A stale commit SHALL be refused with a result code distinct from the codes for a bad argument and a missing layer.

The revision the ABI reports SHALL advance for EVERY wholesale replacement of the layer's triangles, including an undo, a redo and a replayed journal event, and SHALL advance rather than return to an earlier value. A host holding a token against a live adjacency, spatial index or sculpting session gets one answer from it — "this is not the geometry you built over" — and a revision that stood still while undo swapped every vertex and every index makes that answer wrong in the one direction the host cannot detect. It is per-document-instance and is not carried in a saved file.

#### Scenario: One call, one undo step
- **WHEN** a host rebuilds a mesh layer through the document with undo enabled
- **THEN** the layer holds the rebuilt triangles, the report describes them, and the undo depth grew by exactly one

#### Scenario: A cancelled rebuild leaves the layer alone
- **WHEN** a rebuild through the document is cancelled
- **THEN** it returns the cancelled result code, the layer's triangles are unchanged, and no undo step was added

#### Scenario: A stale commit is refused distinctly
- **WHEN** a host commits a rebuild at a revision the layer has moved past
- **THEN** the commit returns a result code distinct from an invalid argument and from a missing layer, and the layer keeps the newer geometry

#### Scenario: Undoing and redoing a rebuild each report a new revision
- **WHEN** a host attaches a mesh layer, rebuilds it, undoes and redoes
- **THEN** the reported revision is strictly greater after each of those steps than before it, and the layer's triangles are the ones that step promised
