# scene-model — a document may carry an imported mesh

Delta for `add-mesh-layers`.

## ADDED Requirements

### Requirement: A layer may be an imported mesh
A layer SHALL optionally be of `mesh` kind, carrying one imported mesh stored beside the document and keyed by layer id rather than inside the evaluated document. Its geometry SHALL be exactly what the importer returned — no welding, reordering, renormalizing or reindexing on the way in — so that what a document carries is what was imported.

Its presence SHALL NOT change what the document evaluates to. A mesh layer SHALL NOT be compiled into a tape, SHALL NOT participate in any blend, and SHALL NOT contribute to influence bounds or per-brick culling. Keeping the geometry out of the evaluated document makes that structural rather than a property to be maintained, as it already is for masks and voxel grids.

A mesh layer SHALL carry the same layer properties as any other: name, order, transform, visibility, ghost and lock. Creating and removing one SHALL go through the command vocabulary, so both are undoable and both serialize with the document.

#### Scenario: Evaluation is unchanged by a mesh layer
- **WHEN** a mesh layer is added to a document and the document is evaluated
- **THEN** the field is bit-identical to the same document without the mesh layer, and every compiled tape is unchanged

#### Scenario: The mesh is carried, not resampled
- **WHEN** a mesh is attached to a document and read back
- **THEN** its positions, normals, colors, uvs and indices are the arrays the importer produced, element for element

#### Scenario: Adding a mesh layer is undoable
- **WHEN** a mesh layer is added and the edit is undone
- **THEN** the layer is gone and the document matches what it was

#### Scenario: Removing a mesh layer does not discard its geometry
- **WHEN** a mesh layer is removed and the removal is undone
- **THEN** the layer returns carrying the same mesh, because the payload is keyed by layer id and is never erased on removal

### Requirement: A mesh layer's transform is applied by whoever consumes it
A mesh layer's vertices SHALL be stored in the space the importer produced, and its `Layer` transform SHALL be applied by whatever reads or exports the mesh rather than baked into the stored geometry. This is the rule voxel content already lives under; a mesh layer states it rather than inheriting it silently.

The transform SHALL be the existing layer transform, edited through the existing command, so that moving a mesh layer is undoable, serializes with the document, and is refused on a locked layer exactly as any other layer edit is.

Unit and axis conversion SHALL be resolved at import by baking a uniform scale into the vertices, as the FBX importer already does when it normalizes to metres. Non-uniform scale is not expressible in a layer transform and SHALL NOT be approximated.

#### Scenario: The stored mesh does not move
- **WHEN** a mesh layer's transform is changed
- **THEN** the stored vertices are unchanged, and an export of that layer places them under the new transform

#### Scenario: A locked mesh layer refuses the edit
- **WHEN** a mesh layer is locked and its transform is set
- **THEN** the edit is refused, as it is for any other locked layer

## MODIFIED Requirements

### Requirement: Document structure
`clay::scene` SHALL model a document as a list of layers, each `voxel`, `sdf` or `mesh` kind, with per-layer transform, visibility, resolution, and material. SDF layers SHALL hold an ordered edit list where each item applies to the combined result of all preceding items. Groups SHALL nest to depth ≥ 4 and carry group ops (including None). Layer instancing SHALL share content by reference such that editing the source updates all instances.

A `mesh` layer carries imported geometry for display and re-export and is not evaluated; it is not an operand and SHALL NOT be instanced.

#### Scenario: Order matters
- **WHEN** an edit list [add sphere, subtract box] is reordered to [subtract box, add sphere]
- **THEN** evaluation produces a different field (subtract-before-add has nothing to remove), demonstrating ordered semantics

#### Scenario: Instance follows source
- **WHEN** a layer is instanced twice and an edit item is added to the source layer
- **THEN** both instances evaluate with the new item without duplicating stored content

#### Scenario: A mesh layer is not an operand
- **WHEN** a mesh layer is instanced
- **THEN** the call is refused, as it already is for a voxel layer
