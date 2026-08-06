# scene-model — control-point curves

Delta for `add-curve-objects`.

## ADDED Requirements

### Requirement: Stroke points carry a type
A stroke point SHALL carry an interpolation type — hard corner, spline, B-spline, or Bezier — defaulting to hard corner. A Bezier point SHALL additionally carry an incoming and an outgoing handle, expressed in the item's local space relative to the point.

A point list SHALL be able to be marked closed, so that the last point connects back to the first.

#### Scenario: A hard point list is the stroke it always was
- **WHEN** a point list whose points are all hard corners is compiled
- **THEN** the tape is bit-identical to the one the same points produced before types existed

#### Scenario: Smooth points curve
- **WHEN** three points are given spline type and the item is evaluated
- **THEN** the surface passes outside the straight chain the same points would produce, and through every control point

#### Scenario: Bezier handles shape the span
- **WHEN** a Bezier point's handles are lengthened
- **THEN** the surface changes, and moving the handles back restores it

#### Scenario: A closed curve joins its ends
- **WHEN** a point list is marked closed
- **THEN** the span between the last point and the first is present in the field

### Requirement: Curves tessellate to a stated tolerance
A curve SHALL be tessellated into the segment chain the stroke opcode evaluates, subdividing a span while its midpoint deviates from its chord by more than the item's tolerance, to a bounded depth. The tolerance SHALL be a property of the document rather than of the host, so that two builds agree on what a document means.

Tessellation SHALL be deterministic: the same control points and tolerance SHALL produce the same segment chain, on every platform and through every binding.

#### Scenario: A tighter tolerance means a closer curve
- **WHEN** the same curve is compiled at a coarse and at a fine tolerance
- **THEN** the fine one uses more segments, and its surface lies closer to the ideal curve

#### Scenario: Tessellation is reproducible
- **WHEN** the same curve is compiled twice
- **THEN** the segment chains are identical

#### Scenario: Subdivision is bounded
- **WHEN** a curve is given a tolerance small enough to demand unbounded subdivision
- **THEN** subdivision stops at the bound rather than growing without limit

### Requirement: Editing a curve is an ordinary edit
Replacing an item's point list SHALL be expressed as a command, so that it is undoable, serializable and refused on a protected layer like every other edit. Its inverse SHALL restore the previous list exactly.

#### Scenario: Editing a curve is undoable
- **WHEN** a curve's points are replaced and the edit is undone
- **THEN** the document is exactly what it was

#### Scenario: A protected layer refuses a curve edit
- **WHEN** a curve on a locked layer has its points replaced
- **THEN** the edit is refused and the curve is unchanged

### Requirement: Curve bounds cover the tessellated curve
An item's bounds SHALL be computed from the tessellated points rather than from the control points, because a spline may pass outside the polygon its control points form. Picking and per-brick culling SHALL therefore not miss a curve that bulges beyond its control points.

#### Scenario: A bulging curve is still picked
- **WHEN** a ray is aimed at the part of a spline that lies outside its control-point hull
- **THEN** the ray reports a hit on that item
