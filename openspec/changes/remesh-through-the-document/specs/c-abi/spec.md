# c-abi — the document-level rebuild

Delta for `remesh-through-the-document`.

## ADDED Requirements

### Requirement: A mesh layer can be rebuilt through the document
The C ABI SHALL expose a rebuild that targets a mesh LAYER: capture, rebuild, validate, replace and record, as one call and one undo step, taking the same versioned parameter descriptor and filling the same versioned report as the pure mesh-to-mesh form.

It SHALL be transactional. Nothing is written until the rebuild has succeeded and validated, so a refusal, a validation failure or a cancellation leaves the layer byte-identical and adds no undo step.

A protected layer SHALL be refused BEFORE the rebuild rather than after it: rebuilding a locked layer for several seconds and then declining to commit is a worse answer than declining immediately.

The ABI SHALL also expose the layer's geometry revision and a revision-checked replacement, so a host that ran the pure rebuild on its own worker thread can commit it without overwriting newer work. A stale commit SHALL be refused with a result code distinct from the codes for a bad argument and a missing layer.

#### Scenario: One call, one undo step
- **WHEN** a host rebuilds a mesh layer through the document with undo enabled
- **THEN** the layer holds the rebuilt triangles, the report describes them, and the undo depth grew by exactly one

#### Scenario: A cancelled rebuild leaves the layer alone
- **WHEN** a rebuild through the document is cancelled
- **THEN** it returns the cancelled result code, the layer's triangles are unchanged, and no undo step was added

#### Scenario: A stale commit is refused distinctly
- **WHEN** a host commits a rebuild at a revision the layer has moved past
- **THEN** the commit returns a result code distinct from an invalid argument and from a missing layer, and the layer keeps the newer geometry
