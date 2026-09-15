## ADDED Requirements

### Requirement: Brick-local edge recording preserves exact mesh output

The brick mesher MAY deduplicate repeated local lattice-edge records before global welding, but SHALL preserve the existing mesh positions, indices, attributes, ordering and per-brick ranges exactly. It SHALL retain cross-brick welding and boundary attribution and SHALL bound additional scratch memory independently of total document size.

#### Scenario: Repeated tetrahedron edges
- **WHEN** multiple tetrahedra in a brick reference the same lattice edge
- **THEN** the recorder may reuse the first local edge record
- **AND** the resulting mesh and ranges are byte-identical to unoptimized recording

#### Scenario: Boundary cells and coarse meshes
- **WHEN** a subset includes straddling triangles or a supported coarse LOD
- **THEN** edge deduplication preserves boundary ownership, topology and exact output

#### Scenario: Unsupported dense lookup
- **WHEN** an edge or brick dimension is outside the bounded dense representation
- **THEN** general recording preserves the established output without an unbounded dense allocation
