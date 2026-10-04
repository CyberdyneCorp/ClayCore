## MODIFIED Requirements

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
bound taken BEFORE the edit and AFTER it, each dilated by the chain pad of the
combines that FOLLOW the operand, by each enclosing group's blend support, and
by the support of every layer fold above it — and unioned over every layer
sharing the content. A caller SHALL union the two sides; one side alone is not
an answer.

The chain pad SHALL be the SUM of the full blend supports (with a feathered
replace's band, an extended mode's own support and a symmetry seam's support)
of the combines that read the running value the operand's combine produced: its
later siblings at every level of the ancestor walk, and the children of any
INLINE group among them, which continue the outer chain. A later non-inline
group SHALL contribute its own combine only. No combine AHEAD of the operand
SHALL be a term, since those built the value its `max(acc, item)` reads and that
value does not change. A sum rather than the largest term, because the drag a
chain applies to its running value accumulates step by step. The chain's
envelope, which the per-brick cull uses, SHALL NOT be used here: it is a fit to
what a cull may drop against an fp16 tolerance, and this region is held to
bricks bit-identical to a rebuild.

#### Scenario: an intersect operand is moved

- **GIVEN** a layer holding a worked form and an Intersect operand at its root
- **WHEN** the operand is moved by a SetTransformCmd
- **THEN** the surface-delta bound covers where the operand was and where it went
- **AND** outside that bound, dilated by the band, no sample changes sign, enters
  the meshing band, or leaves it
- **AND** the node's influence bound still reports the layer's extent

#### Scenario: an operand appended last carries no chain pad

- **GIVEN** a sphere carrying 96 smooth stamps whose blend radii scale with the
  form, and a hard Intersect cylinder appended after them
- **WHEN** the cylinder is moved by a SetTransformCmd
- **THEN** the surface-delta bound, unioned over both sides, is exactly the swept
  geometry bound of the cylinder and its symmetry copies
- **AND** it is the same box at the reference size and at ten times the extent

#### Scenario: smooth combines after the operand

- **GIVEN** an Intersect operand at the head, the middle or the tail of a long
  chain of smooth stamps, under mirror or radial symmetry, inside an inline
  group or ahead of one holding the stamps
- **WHEN** it is moved, and a brick cache is dirtied by the reported region and
  refilled
- **THEN** the cache holds the same bricks, bit for bit, as one rebuilt from
  nothing on the moved document
- **AND** each smooth combine after the operand widens the bound by its support,
  and a combine ahead of it does not

The proof rests on the operand's own field being a DISTANCE outside its geometry
bound. Where the placement is not a similarity — a non-uniform `scale_axes` on
the item or on a layer holding it — the field is short of the true distance by
up to `max(s)/min(s)`, and the engine SHALL report nothing rather than a box the
field does not honour.

#### Scenario: an edit the proof does not cover

- **GIVEN** an Intersect operand that is deformed, gated, unbounded, infinitely
  repeated, a sampled volume, carries a non-uniform per-axis scale or sits in a
  layer that does, or sits in a layer whose chain holds a spatial morph or a
  gate
- **WHEN** it is moved by a SetTransformCmd
- **THEN** no surface-delta bound is reported
- **AND** the caller dirties by the conservative influence bound instead

#### Scenario: an op that is not Intersect

- **GIVEN** a Subtract, Add or Paint item
- **WHEN** it is moved by a SetTransformCmd
- **THEN** no surface-delta bound is reported, because the node's influence bound
  is already that box
