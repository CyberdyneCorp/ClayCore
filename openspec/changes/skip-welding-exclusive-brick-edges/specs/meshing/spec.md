## ADDED Requirements

### Requirement: Exclusive ordinary edges bypass global welding safely
The mesher SHALL avoid global welding lookups for proven exclusive locally deduplicated ordinary brick edges while preserving complete mesh output.

#### Scenario: Unique bounded ordinary bricks
- GIVEN distinct requested bricks of dimension 1 through 16 within the canonical coordinate-packing domain and no straddlers
- WHEN ordinary edges are replayed after local deduplication
- THEN an edge wholly exclusive to its brick SHALL be emitted without a global table lookup
- AND all boundary edges SHALL remain globally welded
- AND positions, indices, attributes and brick ranges SHALL match the unoptimized reference exactly

#### Scenario: Unsupported ownership proof
- GIVEN duplicate requested keys, unsupported dimensions, out-of-domain coordinates, straddlers or unoptimized reference recording
- WHEN the mesh is produced
- THEN every edge SHALL retain the original global-welding behavior
