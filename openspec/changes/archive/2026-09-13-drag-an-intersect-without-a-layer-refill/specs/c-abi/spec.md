# c-abi

## ADDED Requirements

### Requirement: a transform edit can report the region it changed

The ABI SHALL provide `clay_layer_set_transform_bound`: the edit
`clay_layer_set_transform` applies, plus the world-space box outside which that
edit did not change the surface or the band around it. It SHALL report the box
in the three states `clay_layer_node_influence_bound` uses (no bounds, a finite
box, unbounded), and SHALL be usable as the region for
`clay_brick_cache_mark_dirty`.

The box SHALL be the swept surface delta where the engine can prove one, and the
conservative influence bound otherwise; a caller SHALL NOT be able to tell which
it received, since both are safe to dirty.

`clay_layer_node_influence_bound` and `clay_brick_cache_mark_dirty_nodes` SHALL
be unchanged: they answer for an arbitrary edit to a node, which for an Intersect
is still the layer's extent.

#### Scenario: dragging an intersect operand

- **GIVEN** a document whose layer holds an Intersect operand
- **WHEN** the operand is moved with `clay_layer_set_transform_bound`
- **THEN** the reported box is far smaller than the layer's extent
- **AND** a brick cache dirtied by it and refilled holds the same bricks, values
  and triangles as a cache rebuilt from nothing on the moved document

#### Scenario: an edit outside the proof domain

- **GIVEN** an operand whose delta the engine cannot prove local
- **WHEN** it is moved with `clay_layer_set_transform_bound`
- **THEN** the reported box is the conservative influence bound
- **AND** the edit itself is applied and recorded exactly as
  `clay_layer_set_transform` applies and records it
