# python-bindings — one geometry revision, shared with the C ABI

Delta for `own-the-mesh-invalidation-signal`.

## MODIFIED Requirements

### Requirement: A document rebuilds one of its mesh layers
`pyclay` SHALL expose, on the document, a rebuild of one mesh layer that returns the report as named values; the layer's geometry revision; and a revision-checked replacement for a caller that ran the pure rebuild itself.

A stale commit and a refused rebuild SHALL raise with messages naming which contract refused them, rather than returning silently.

A sculpting session held over a layer that has since been rebuilt SHALL raise on its next operation, including when the replacement had the same vertex and triangle counts.

The revision `pyclay` reports SHALL be the SAME quantity the C ABI reports for the same document — one counter kept beside the triangles, not a second one maintained in parallel — so the two bindings cannot answer differently about the same layer. It SHALL advance for every wholesale replacement, including an undo, a redo and a replayed journal event.

#### Scenario: The layer rebuild is one undo step from Python
- **WHEN** a mesh layer is rebuilt from Python with undo enabled
- **THEN** the layer holds the new triangles, the returned report carries the stage timings and the surface distance, and one undo restores the previous triangles

#### Scenario: A stale commit raises
- **WHEN** a caller commits a rebuild at a revision the layer has moved past
- **THEN** it raises, and the layer keeps the newer geometry

#### Scenario: Undo, redo and replay each advance the revision from Python
- **WHEN** a mesh layer is rebuilt, undone and redone, and a journal carrying a rebuild is replayed onto a document holding the earlier triangles
- **THEN** the revision is strictly greater after each of those steps, and the layer holds the triangles that step promised

#### Scenario: Nothing that is not a replacement moves it
- **WHEN** the layer is sculpted, transformed, hidden and shown, protected, or a different mesh layer is rebuilt and undone
- **THEN** this layer's revision is unchanged throughout
