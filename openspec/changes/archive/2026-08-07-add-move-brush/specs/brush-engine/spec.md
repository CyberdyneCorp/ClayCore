# brush-engine — the Move brush

Delta for `add-move-brush`.

## ADDED Requirements

### Requirement: A world drag resolves into a field-level move
The library SHALL resolve a world-space centre, radius and displacement into the per-item warps that reproduce that drag on the ASSEMBLED surface of a layer, rather than on one item of it. The result SHALL be returned rather than applied, so a host can preview a drag before committing it and decide which commands carry it — the same rule the stroke engine's node consumer follows.

Each warp SHALL be expressed in its item's OWN frame, mapping the world centre, radius and displacement through that item's world transform — which is the layer's transform composed with the item's, because a group's transform does not reach its children in this scene model. The resolver SHALL agree with the evaluator on that point rather than accumulating a chain the evaluator does not.

Each warp SHALL be marked for the FRONT of its item's chain, because a warp appended behind an existing deformer has its region weight evaluated at a point that deformer already moved.

An item whose influence bounds do not reach the drag's region SHALL receive no warp at all: a deformer with finite support, outside its own support, is a no-op that still costs a tape record on every evaluation.

#### Scenario: A blended form moves as one surface
- **WHEN** a drag centred between two smooth-unioned items is resolved and applied
- **THEN** both sides lift, symmetrically, and the lift peaks at the world centre

#### Scenario: Grabbing one item is not the same thing
- **WHEN** the same drag is expressed as a grab on a single item instead
- **THEN** that item's side moves and the other is left behind

#### Scenario: A nested item moves where the drag was aimed
- **WHEN** the layer's items sit under a group and a drag is resolved
- **THEN** the surface moves where the drag was aimed in WORLD space, matching what the evaluator does with those items

#### Scenario: A transformed layer maps correctly
- **WHEN** the layer carries a transform and a drag is resolved in world space
- **THEN** the surface moves where the drag was aimed, not where it would have landed in layer space

#### Scenario: Items out of reach are skipped
- **WHEN** a layer holds items far outside the drag's radius
- **THEN** no warp is produced for them

#### Scenario: Nothing is written
- **WHEN** a drag is resolved
- **THEN** the document is unchanged until the caller applies the result

### Requirement: The Move brush inherits grab's pull, and says so
The move SHALL use the existing `grab` deformation rather than a new one, and SHALL therefore inherit its behaviour: the surface moves LESS than the displacement asked for, because the region weight is taken at the sample point rather than at its preimage.

This SHALL be documented rather than corrected. Solving for the true preimage costs an iteration per sample and buys nothing a sculptor can feel, and the pull is monotonic in the displacement, so a UI can calibrate against it.

#### Scenario: The pull is monotonic
- **WHEN** the same drag is resolved at increasing displacements
- **THEN** the surface moves further each time, by less than the displacement given
