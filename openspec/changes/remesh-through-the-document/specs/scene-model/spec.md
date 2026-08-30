# scene-model — a rebuild through the document

Delta for `remesh-through-the-document`.

## ADDED Requirements

### Requirement: A whole-mesh replacement is one undo step
The undo history SHALL be able to record the replacement of one mesh layer's entire geometry as a single reversible step, carrying the mesh on each side of the replacement.

A sparse vertex delta SHALL NOT be used for this. A delta records no indices — that is the fixed-topology contract it exists to serve — so it cannot express a change that replaces every vertex and every polygon, and applying one recorded against the old geometry to the new geometry is a corruption rather than an undo.

The step's cost SHALL be counted by the history's byte accounting, so the budget that evicts oldest steps can see the largest kind there is. The step SHALL survive a crash through the journal, so a recovered document does not silently lack a rebuild the artist watched happen.

A replacement that changed neither the vertex count, the triangle count, nor any position SHALL be dropped rather than recorded, as every other recorder drops a no-op.

#### Scenario: One rebuild is one step, and it reverses exactly
- **WHEN** a mesh layer is rebuilt through the document with undo enabled
- **THEN** the undo depth grows by exactly one, undoing restores the layer's previous triangles, and redoing restores the rebuilt ones
- **AND** undoing a second time after a redo works, because applying the step did not consume it

#### Scenario: A refused or cancelled rebuild adds no step
- **WHEN** a rebuild is refused for a resource budget, refused for a protected layer, or cancelled
- **THEN** the layer's triangles are unchanged and the undo depth is unchanged

### Requirement: A mesh layer carries a geometry revision
A document SHALL expose, per mesh layer, a revision that changes when the layer's geometry is REPLACED wholesale and does not change when the layer is sculpted.

The distinction is the point. A vertex-displacement brush leaves the topology alone, which is what lets an adjacency, a spatial index or a live sculpting session remain valid across it; a wholesale replacement invalidates all three. A revision that moved for both would force every consumer to rebuild after every brush stroke, and one that moved for neither would not exist.

#### Scenario: A sculpt does not move it and a rebuild does
- **WHEN** a mesh layer is stamped with a displacement brush and then rebuilt
- **THEN** the revision is unchanged after the stamp and changed after the rebuild

### Requirement: A stale result cannot overwrite newer work
Where a caller performs a long rebuild outside the document and commits the result afterwards, the commit SHALL accept the revision the caller read before starting, and SHALL be refused if the layer's geometry has been replaced since.

A refusal SHALL leave the layer byte-identical. A caller that knows nothing could have intervened MAY skip the check explicitly; skipping it SHALL be something the caller asks for rather than the default that happens when it forgets.

#### Scenario: The commit that arrived late is refused
- **WHEN** a caller reads a layer's revision, rebuilds outside the document, the layer is rebuilt by something else in the meantime, and the caller then commits
- **THEN** the commit is refused, and the layer still holds the newer geometry

#### Scenario: The same commit at the current revision is accepted
- **WHEN** the caller re-reads the revision and commits at it
- **THEN** the commit succeeds

### Requirement: A cached surface session is refused after its layer is rebuilt
A sculpting session held over a mesh layer SHALL refuse to operate once that layer's geometry has been replaced, including when the replacement happens to have the same vertex and triangle counts.

Comparing counts is not sufficient and neither is comparing the address of the layer's mesh: a replacement that lands on the same counts passes both, and the session's cached adjacency and spatial index then describe triangles that no longer exist. Every operation after that moves the wrong vertices with no error.

#### Scenario: A same-count replacement is still caught
- **WHEN** a mesh layer is replaced by a mesh with identical vertex and index counts but different connectivity, under a live sculpting session
- **THEN** the next operation on that session is refused rather than applied
