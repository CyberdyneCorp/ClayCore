# python-bindings — the document-level rebuild

Delta for `remesh-through-the-document`.

## ADDED Requirements

### Requirement: A document rebuilds one of its mesh layers
`pyclay` SHALL expose, on the document, a rebuild of one mesh layer that returns the report as named values; the layer's geometry revision; and a revision-checked replacement for a caller that ran the pure rebuild itself.

A stale commit and a refused rebuild SHALL raise with messages naming which contract refused them, rather than returning silently.

A sculpting session held over a layer that has since been rebuilt SHALL raise on its next operation, including when the replacement had the same vertex and triangle counts.

#### Scenario: The layer rebuild is one undo step from Python
- **WHEN** a mesh layer is rebuilt from Python with undo enabled
- **THEN** the layer holds the new triangles, the returned report carries the stage timings and the surface distance, and one undo restores the previous triangles

#### Scenario: A stale commit raises
- **WHEN** a caller commits a rebuild at a revision the layer has moved past
- **THEN** it raises, and the layer keeps the newer geometry
