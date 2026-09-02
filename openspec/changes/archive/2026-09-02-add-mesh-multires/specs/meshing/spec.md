# meshing — the hierarchy and the interchange mesh

Delta for `add-mesh-multires`.

## ADDED Requirements

### Requirement: A hierarchy exports through the interchange mesh
`mesh::Mesh` SHALL remain the flat interchange format and SHALL NOT gain hierarchy, level or detail members. A multiresolution surface SHALL export a level as an ordinary mesh, and every existing producer and consumer SHALL continue to see exactly the arrays it saw before.

Attribute transfer SHALL keep its current contract — it moves colours, UVs and normals and moves no position. Geometry and detail reprojection SHALL be a separate operation sharing the same spatial query and not the same guarantee.

The fixed-topology sculptor SHALL be unaffected. A mesh layer that carries no hierarchy behaves exactly as it did.

#### Scenario: An exported level is an ordinary mesh
- **WHEN** a level is exported and passed to validation, decimation, an exporter and the readback accessors
- **THEN** each accepts it unchanged

#### Scenario: Attribute transfer still moves no position
- **WHEN** attribute transfer runs after this change
- **THEN** the target's positions are byte-identical to before it
