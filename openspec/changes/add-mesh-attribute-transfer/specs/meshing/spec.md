# meshing

## ADDED Requirements

### Requirement: Attribute transfer between meshes
The library SHALL transfer per-vertex attributes from one mesh to another by closest point: for each vertex of the target, the nearest point on the source surface SHALL be found and the source's attributes there interpolated by that point's barycentrics.

This exists because everything that leaves a mesh layer loses what a mesh layer was holding. Sampling a mesh into a field and meshing it back preserves the shape and discards the colours and uvs, and that is the price of any trip through the field — a boolean, a consolidation, a level change. The nearest point on the original surface knows what belonged there, and the ray tree already returns the triangle and barycentrics needed to read it.

Colours and uvs SHALL transfer by default. Normals SHALL NOT, unless the caller asks: a resampled mesh has its own geometry and its normals should describe it, so taking the source's would make new geometry shade like the old shape.

Positions and topology SHALL NOT be modified. This is an attribute transfer and not a projection: a verb that moved the target's vertices toward the source would be a different operation, and conflating the two turns "transfer" into "deform" without saying so.

A target vertex farther from the source than a caller-supplied threshold SHALL take a documented fallback rather than the attribute of whatever was nearest. Geometry can exist where the source never was — after a boolean, or where a mesher bridged a gap — and the closest point to it carries no meaning. The call SHALL report how many vertices transferred and how many fell back, because a result that fell back across most of the mesh is otherwise indistinguishable from a good one.

Transferring a mesh's attributes onto ITSELF SHALL return them bit-identically.

Transfer SHALL be DETERMINISTIC: the same pair of meshes SHALL produce the same attributes on every run and every platform.

**The uv seam limitation SHALL be documented rather than left to be discovered.** Uvs are per VERTEX, which is how a seam is represented: the source duplicates a position into two vertices carrying different uvs. A target vertex lying on such a seam has one uv slot and two correct answers, and will take whichever triangle the closest-point query returned — which can stretch a triangle across the uv layout. Colour is unaffected, being continuous across a seam. This follows from per-vertex uvs and is a property of the operation, not a defect in it.

**Attribute transfer SHALL NOT be described as recovering a round trip.** It refunds the paint and most of the uvs. It does not refund the topology: the target is still the mesher's geometry, with new vertices and no relationship to the retopology that went in.

#### Scenario: An identity transfer is exact
- **WHEN** a mesh's attributes are transferred onto a copy of itself
- **THEN** every colour and uv is bit-identical, and positions and indices are unchanged

#### Scenario: Colour survives a trip through the field
- **WHEN** a coloured mesh is sampled into a field, meshed back, and given the original's attributes
- **THEN** the colours approximate the original's across the surface, while the topology remains the mesher's

#### Scenario: Geometry the source never occupied
- **WHEN** a target vertex lies farther from the source than the threshold
- **THEN** it takes the documented fallback, and the reported fallback count includes it

#### Scenario: Nothing moves
- **WHEN** attributes are transferred by any options
- **THEN** the target's positions and indices are byte-identical before and after
