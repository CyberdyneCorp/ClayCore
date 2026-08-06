# c-abi — a volume producer

Delta for `add-mesh-to-field-import`.

## ADDED Requirements

### Requirement: The C ABI can build a volume from a mesh
`add-sampled-fields` declared `CLAY_PRIM_VOLUME` but refused to construct one, because nothing in the C ABI could supply the samples. Mesh import supplies them, so the C ABI SHALL provide a producer that samples a mesh into an item carrying a volume, and construction SHALL no longer be refused.

#### Scenario: A mesh becomes an item through the C ABI
- **WHEN** a C caller loads a mesh and samples it into an item
- **THEN** the item carries a volume and evaluates as the mesh's shape

#### Scenario: A volume item survives the round trip
- **WHEN** a document containing a C-built volume item is saved and reloaded
- **THEN** the field is unchanged

#### Scenario: Degenerate input is refused where the item is built
- **WHEN** a C caller samples a mesh with no triangles, or passes a non-positive cell size
- **THEN** the call fails with an invalid-argument error
