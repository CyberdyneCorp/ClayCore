# scene-model — the generation a wholesale mesh replacement advances

Delta for `own-the-mesh-invalidation-signal`.

## MODIFIED Requirements

### Requirement: A mesh layer carries a geometry revision
A document SHALL expose, per mesh layer, a revision that changes when the layer's geometry is REPLACED wholesale and does not change when the layer is sculpted.

The distinction is the point. A vertex-displacement brush leaves the topology alone, which is what lets an adjacency, a spatial index or a live sculpting session remain valid across it; a wholesale replacement invalidates all three. A revision that moved for both would force every consumer to rebuild after every brush stroke, and one that moved for neither would not exist.

EVERY path that replaces a layer's triangles SHALL advance it, including the ones the document does not initiate: an attach, a rebuild through the document, a weld that changed something, an undo, a redo, and a replayed journal event. A restoration path that put the triangles back without advancing the revision is the failure this requirement exists to forbid — a consumer's cached adjacency, spatial index or sculpting session is then describing geometry the document no longer holds, and nothing else can detect it.

The revision SHALL ADVANCE on an undo rather than being restored to the value it held when those triangles were last current. It is an invalidation token for a consumer's LIVE caches, not the age of the restored mesh: a consumer that built a cache over the replacement and then undid holds a cache for geometry that is gone, and a revision handed back to an earlier value would tell it the opposite. Monotonic advance is the only rule under which "my token differs from the document's" means "rebuild".

A replacement that is REFUSED SHALL NOT advance it, and neither SHALL an undo or redo that had nothing to reverse. Everything that is not a replacement of the triangles SHALL leave it alone: a sculpt, a rename, a visibility or protection change, a transform-only edit, and history affecting a different layer.

The revision is per-session state and SHALL NOT be serialized. Nothing it invalidates — an adjacency, a spatial index, a live sculpting session — survives a save and a reopen, so there is nothing on the other side of a load for a restored number to protect; a reopened document establishes a fresh generation domain. Writing it would also make a saved document's bytes depend on how the session reached that geometry, which the round trip's identity forbids.

#### Scenario: A sculpt does not move it and a rebuild does
- **WHEN** a mesh layer is stamped with a displacement brush and then rebuilt
- **THEN** the revision is unchanged after the stamp and changed after the rebuild

#### Scenario: Undo and redo each advance it, and restore the triangles they promise
- **WHEN** a mesh layer is attached, rebuilt, undone and redone
- **THEN** the revision strictly advances at each of the four transitions
- **AND** the layer holds the attached triangles after the undo and the rebuilt ones after the redo

#### Scenario: Repeated cycles never repeat a generation
- **WHEN** undo and redo are alternated several times over one replacement
- **THEN** every transition produces a revision greater than the one before it

#### Scenario: A replayed journal advances it too
- **WHEN** a journal containing a wholesale replacement is replayed onto a document holding the layer's earlier triangles
- **THEN** the layer holds the replacement's triangles and the revision has advanced

#### Scenario: Nothing else moves it
- **WHEN** the layer is renamed, hidden and shown, transformed, protected, or a DIFFERENT mesh layer is rebuilt and undone
- **THEN** this layer's revision is unchanged throughout

#### Scenario: A refusal spends no generation
- **WHEN** a replacement is refused for a stale expected revision or a protected layer, or an undo is asked for with nothing left to reverse
- **THEN** the layer's triangles and its revision are both unchanged
