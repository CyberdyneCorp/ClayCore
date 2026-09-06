# file-io — a `.clayspace` carries a multires hierarchy

Delta for `persist-a-multires-hierarchy`.

## ADDED Requirements

### Requirement: A multires hierarchy is a document chunk
`.clayspace` SHALL carry a multiresolution hierarchy as its own chunk, one per hierarchy, naming the layer id it belongs to and carrying the bytes `mesh::MultiresSurface::encode()` produces. The chunk SHALL be gated on the container minor so that a reader predating it skips it rather than refusing the document, and the surface encoding SHALL be used as it stands rather than re-specified here — the surface already versions itself, and a second version negotiated in the container would be two places to get the same answer wrong.

A hierarchy SHALL live beside the document keyed by layer id, as voxel grids, masks and imported meshes already do, and SHALL NOT be reachable from `clay::scene`. The layering table withholds `clay/mesh` from `clay::scene`, which is what makes "a hierarchy does not change what the document evaluates to" structural rather than a rule to maintain.

#### Scenario: A hierarchy round trips
- **WHEN** a document holding a mesh layer with a multires hierarchy is saved and reloaded at the current minor
- **THEN** the reloaded hierarchy has the same level count, the same sculpt and display levels, and every level evaluates to a surface bit-identical to the one saved

#### Scenario: The detail above the cage survives
- **WHEN** a hierarchy carrying sculpted detail at a level above its base is saved and reloaded
- **THEN** the detail field and the sculpt-layer stack come back, and the reloaded surface is not the flat cage

#### Scenario: A reader predating the chunk is unaffected
- **WHEN** a document containing a hierarchy chunk is opened by a reader at the previous minor
- **THEN** the document loads, the mesh layer holding the cage loads, and the hierarchy chunk is skipped rather than refused

### Requirement: Writing at the previous minor says what it drops
Writing a document at the minor below the hierarchy minor SHALL omit the hierarchy chunk and otherwise reproduce exactly the bytes that minor produced, and the release notes SHALL state what the downgrade loses: the levels, the detail field and the sculpt-layer stack — everything above the base cage, which is kept as an ordinary mesh layer.

A document that carries no hierarchy SHALL be byte-identical at both minors, so the cost of this feature to a document that does not use it is nothing.

#### Scenario: Writing at the previous minor drops only the hierarchy
- **WHEN** a document holding a hierarchy is written at the minor below the hierarchy minor
- **THEN** the bytes are exactly what that minor produced before hierarchies existed, and reloading them yields the base cage as a mesh layer with no hierarchy

#### Scenario: A document with no hierarchy is unchanged
- **WHEN** a document carrying no hierarchy is written at the current minor and at the previous one
- **THEN** the two files are byte-identical

### Requirement: An orphaned hierarchy chunk is harmless
A hierarchy entry SHALL survive the removal of the layer it names, because the inverse of a layer removal restores a `Layer` by value and cannot carry a payload — the same reason `mesh_layers` keeps its entries. The writer SHALL emit a chunk only for an id that is still a mesh layer, and the reader SHALL drop a chunk naming a layer the document does not hold.

#### Scenario: Undo within a session keeps the hierarchy
- **WHEN** a mesh layer carrying a hierarchy is removed and the removal is undone
- **THEN** the hierarchy is still associated with the restored layer

#### Scenario: A removed layer's hierarchy is not written
- **WHEN** a document is saved after a hierarchy's layer has been removed and the removal has not been undone
- **THEN** no chunk is written for that id, and reloading yields a document with no orphaned hierarchy
