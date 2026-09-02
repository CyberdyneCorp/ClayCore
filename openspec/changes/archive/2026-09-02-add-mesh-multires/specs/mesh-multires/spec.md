# mesh-multires

Delta for `add-mesh-multires`. A new capability: a subdivision hierarchy over a
mesh, carrying detail that survives edits to the form beneath it.

## ADDED Requirements

### Requirement: A level is its parent subdivided plus its own detail
The library SHALL represent a multiresolution surface as a base mesh, a deterministic subdivision hierarchy, and per-level detail, where a level's positions are its subdivided parent's positions PLUS that level's detail.

Levels SHALL NOT be stored as unrelated absolute meshes. Without the relationship a change to a lower level has no defined effect on a higher one, so either the fine detail or the coarse edit must be discarded — and preserving both is the whole purpose of the hierarchy.

Level generation SHALL be DETERMINISTIC: the same base mesh and the same rule SHALL produce the same hierarchy on every platform and every run.

A level SHALL be exportable as an ordinary `mesh::Mesh`, so that every existing consumer — meshers, exporters, validation, the readback accessors — sees a flat mesh and needs no knowledge of the hierarchy.

#### Scenario: A level exports as an ordinary mesh
- **WHEN** any level of a hierarchy is exported
- **THEN** the result is a flat interchange mesh that existing consumers accept unchanged

#### Scenario: Level generation is deterministic
- **WHEN** the same base mesh is subdivided to the same level twice, on any platform
- **THEN** the resulting positions and connectivity are identical

### Requirement: Detail is stored in a transported local frame
Detail SHALL be stored as coefficients in a local frame — tangent, bitangent and normal — derived from the subdivided parent surface, and SHALL NOT be stored only as a world-space offset.

A world-space offset is adequate for small changes and fails at the case the feature exists for: when the parent surface rotates or bends, a stored world vector no longer points along the surface it belongs to, and detail shears away from the form that carried it.

The frame SHALL be TRANSPORTED rather than rebuilt from whichever neighbour is encountered first: a UV tangent where a valid parametrization exists, a deterministic geometric tangent otherwise, rotated by the shortest arc when the parent normal moves, with sign consistency enforced against the previous frame. An unstable frame rotates detail, and the artefact appears in a render rather than in a numeric test.

Detail SHALL be authoritative in single precision. It SHALL NOT be quantized in this change: high-frequency detail is where a visible artefact appears first, and any compression waits on a measured error bound.

#### Scenario: A wrinkle survives a bend
- **WHEN** detail is sculpted at a fine level and the parent surface is then bent at a coarse one
- **THEN** the detail remains attached to the surface, in the same local orientation, rather than shearing away from it

#### Scenario: A small deformation does not flip the frame
- **WHEN** a parent surface is deformed slightly and the frames are rebuilt
- **THEN** no frame reverses sign, and the reconstructed detail does not rotate

### Requirement: The sculpt level is independent of the display level
The library SHALL allow the level a brush writes to and the level a host displays to be set independently.

Editing a lower level while displaying a higher one SHALL reconstruct the higher levels through the stored relationship, preserving each level's persistent detail.

Sculpting at a level SHALL write only that level's persistent data. Higher levels' detail SHALL NOT be rewritten merely because their reconstructed world positions moved.

#### Scenario: A coarse edit is seen at the fine level
- **WHEN** the sculpt level is coarse, the display level is fine, and a broad edit is made
- **THEN** the displayed surface shows the broad change and retains the fine detail

#### Scenario: Returning to a fine level loses nothing
- **WHEN** detail is sculpted at a fine level, a coarse level is edited, and the fine level is made active again
- **THEN** the fine level's detail coefficients are unchanged

### Requirement: Propagation is local
A change at a level SHALL propagate to the descendants of the vertices it touched and SHALL NOT reconstruct whole levels.

Normals SHALL be recomputed for the changed region at the active level and for the propagated regions above it, never for a whole level.

Per-level runtime state — adjacency and the spatial index — SHALL be built lazily for the levels in use, keyed on the level's revision, and SHALL be droppable without touching authoritative detail.

#### Scenario: A dab does not rebuild a level
- **WHEN** a small dab is made at a coarse level of a deep hierarchy
- **THEN** the vertices reconstructed at each higher level are those descending from the touched vertices, and the cost is measured against a cold full reconstruction rather than asserted

#### Scenario: Dropping a cache costs nothing authoritative
- **WHEN** the runtime caches of inactive levels are released and then rebuilt
- **THEN** the reconstructed surface is identical and the detail checksum is unchanged

### Requirement: Adding a level reports its cost before paying it
Adding a level SHALL estimate the memory it will require — positions, detail, normals, indices, and any spatial index it would build — and SHALL refuse with a typed budget error rather than allocating part of it.

Adding and removing a level SHALL be build-then-publish: a failure or a cancellation SHALL leave the surface exactly as it was.

Subdivision multiplies faces, so it is the peak allocation rather than the steady state that fails on a memory-constrained device, and a host needs the number before it commits.

#### Scenario: An over-budget level is refused, not half-built
- **WHEN** a level whose predicted cost exceeds the budget is requested
- **THEN** the call fails with a typed error and the surface is unchanged

#### Scenario: A cancelled level leaves nothing behind
- **WHEN** adding a level is cancelled part way
- **THEN** the surface is byte-identical to before the call

### Requirement: A hierarchy carrying detail refuses arbitrary topology change
Once a hierarchy contains sculpted detail, an arbitrary change to its base connectivity SHALL be refused rather than applied.

The relationship between a level and its parent is defined by the subdivision stencils; changing the base connectivity makes every stencil above it meaningless, and detail stored against them is no longer interpretable.

The supported route SHALL be an explicit conversion: a new base, subdivided, with the previous surface's detail projected onto it. That conversion SHALL be a named operation with a stated cost, not an implicit consequence of an edit.

Geometry and detail projection SHALL be a separate operation from attribute transfer. Attribute transfer promises it moves no position, and that promise SHALL NOT be weakened to accommodate reprojection.

#### Scenario: A base edit is refused
- **WHEN** an arbitrary topology change to the base is attempted on a hierarchy that carries detail
- **THEN** it is refused with a typed error naming the conversion that is supported

#### Scenario: Detail survives an explicit reprojection
- **WHEN** a sculpted surface is projected onto a new base and a hierarchy is rebuilt from it
- **THEN** the reconstructed fine level matches the source within the stated tolerance

### Requirement: Multires undo records what was edited
The library SHALL record a multires gesture as the persistent data it changed — the level, the entries, and their values before and after — coalesced over the gesture.

Reconstructed positions at higher levels are derived state and SHALL NOT be recorded. Recording them would multiply an undo step by the number of levels and carry no information the relationship does not already supply.

Reverting SHALL restore the surface exactly, and the history SHALL reach the surface through a resolver supplied by the owner above it, as the existing kinds do.

#### Scenario: Undo size follows the edit, not the hierarchy
- **WHEN** a gesture at a coarse level of a deep hierarchy is recorded
- **THEN** the recorded bytes follow the vertices edited at that level and do not scale with the number of levels above it

#### Scenario: A multires stroke reverts exactly
- **WHEN** a stroke at any level is reverted
- **THEN** the surface reconstructs bit-identically to before the stroke
