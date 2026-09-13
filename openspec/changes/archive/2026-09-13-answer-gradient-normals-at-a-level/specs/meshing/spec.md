## ADDED Requirements

### Requirement: A coarse level can be shaded from the field

Gradient normals SHALL be answered at every level a cache can mesh, not only at
the full-resolution one.

A host drawing a coarse surface otherwise has only normals derived from the
triangles, and on a coarse lattice those differ from the field's own gradient by
enough to be visible — a coarse surface is then face-shaded by construction
rather than by choice.

**The evaluation SHALL differ by level, and the difference is the point.** At
full resolution the attributes are evaluated through per-brick culled tapes, so
their cost follows the bricks named. At a coarser level they SHALL be evaluated
through the whole document's field, because a coarse vertex does not sit on the
field's surface, and a culled tape agrees with the whole document's only where
both are in band. Out there the two are both clamped rather than equal, and a
bound derived from the clamped region is flat where the field is not.

**A level's attribute pass therefore SHALL NOT be required to follow the bricks
named**, and that cost SHALL be stated rather than discovered.

Per-vertex COLOUR SHALL remain refused above the full-resolution level, for a
reason the gradient does not share: a coarse level carries no colour samples of
its own, and nothing supplies them. A refusal SHALL NOT be downgraded into an
approximation.

#### Scenario: A coarse level shaded from the field
- **WHEN** a coarse level is meshed with gradient normals and a document
- **THEN** it returns a mesh whose normals follow the field rather than the triangles

#### Scenario: Colour at a coarse level
- **WHEN** a coarse level is meshed with per-vertex colour asked for
- **THEN** the call is refused rather than answered with an approximation

#### Scenario: The full-resolution level is unchanged
- **WHEN** the full-resolution level is meshed with gradient normals
- **THEN** it is evaluated as it was before, through per-brick culled tapes

#### Scenario: Normals from the triangles still work everywhere
- **WHEN** a level is meshed with normals derived from the triangles and no document
- **THEN** it succeeds at every level the cache can mesh
