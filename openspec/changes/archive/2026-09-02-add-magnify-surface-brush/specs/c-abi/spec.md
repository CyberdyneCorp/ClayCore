## ADDED Requirements

### Requirement: A magnify on the assembled surface is reachable from C
The C ABI SHALL expose the surface magnify as `clay_layer_magnify_surface`, with a `_preview` counterpart and a versioned `clay_magnify_params`, on the contract `clay_layer_move_surface` already has: it resolves against every item the region reaches, maps the gesture into each item's frame, makes the whole gesture ONE undo step, and issues ONE invalidation for it.

The strength SHALL be a separate argument rather than a field of the params struct, because a live gesture holds its region fixed and grows only the strength.

The gesture SHALL invalidate its own ball, one per image the layer's symmetry makes of it, with NO dilation — outside the radius the region weight is zero and the point is returned unchanged, for either sign of the strength — widened by each sharer's influence bound when the edit list is instanced.

A strength of zero SHALL be refused rather than accepted as a no-op: it scales by one, so it is not a gesture, and a host that reached the call by accident should hear about it at the boundary.

#### Scenario: A gesture reaches every contributing item
- **WHEN** clay_layer_magnify_surface is called over a form built from two blended items
- **THEN** it reports two items applied and the surface changes symmetrically about the gesture's centre

#### Scenario: One undo step
- **WHEN** a magnify that warps several items is applied with undo enabled
- **THEN** the undo depth grows by one, and undoing it restores the field exactly

#### Scenario: A gesture that reaches nothing succeeds
- **WHEN** the region is far from any item
- **THEN** the call returns OK, reports zero applied, and the document is unchanged

#### Scenario: Malformed gestures are refused
- **WHEN** the call is made with a null centre or params, an unknown layer, a non-positive radius, a strength of zero, or a params struct whose struct_size is stale
- **THEN** it is refused, and the preview refuses the same cases
