# scene-model

## ADDED Requirements

### Requirement: an edit's surface delta is a separate question from a node's influence

The engine SHALL answer two different questions about an edit, and SHALL NOT
answer either with the other.

`node_influence_bound_in_document` answers WHERE A NODE CAN CHANGE THE FIELD.
For an Intersect item that is the extent of its layer, because `max(acc, item)`
is the item's own value everywhere the item is not, and that answer SHALL NOT be
narrowed: the public influence queries, per-brick culling, the generic
dirty-node API and every edit kind other than the one below depend on it.

`command_surface_delta_bound` answers WHERE ONE PARTICULAR EDIT CAN CHANGE THE
BAND-CLAMPED FIELD, given the document before it and after it. It SHALL be
available only for a `SetTransformCmd` on an existing, visible Intersect item
whose support is finite, and SHALL report nothing for every other command, node
and op — so that no other edit's dirty region changes at all.

Where it answers, the region SHALL be the union of the operand's own geometry
bound taken BEFORE the edit and AFTER it, each dilated by the pad its layer's
chain needs (`cull_pad`), by each enclosing group's blend support, and by the
support of every layer fold above it — and unioned over every layer sharing the
content. A caller SHALL union the two sides; one side alone is not an answer.

#### Scenario: an intersect operand is moved

- **GIVEN** a layer holding a worked form and an Intersect operand at its root
- **WHEN** the operand is moved by a SetTransformCmd
- **THEN** the surface-delta bound covers where the operand was and where it went
- **AND** outside that bound, dilated by the band, no sample changes sign, enters
  the meshing band, or leaves it
- **AND** the node's influence bound still reports the layer's extent

#### Scenario: an edit the proof does not cover

- **GIVEN** an Intersect operand that is deformed, gated, unbounded, infinitely
  repeated, a sampled volume, or sits in a layer whose chain holds a spatial
  morph or a gate
- **WHEN** it is moved by a SetTransformCmd
- **THEN** no surface-delta bound is reported
- **AND** the caller dirties by the conservative influence bound instead

#### Scenario: an op that is not Intersect

- **GIVEN** a Subtract, Add or Paint item
- **WHEN** it is moved by a SetTransformCmd
- **THEN** no surface-delta bound is reported, because the node's influence bound
  is already that box
