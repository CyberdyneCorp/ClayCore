# voxel-engine

## ADDED Requirements

### Requirement: The crease verb
The voxel engine SHALL provide a `crease` sculpting verb — the V-groove that cuts wrinkles, seams and folds, and raises the crisp ridge when inverted. It exists on the SDF side as `Op::Incise` and on a mesh layer as the `crease` brush; a voxel layer is where free-form sculpting happens and is the representation that could not cut one.

The verb SHALL apply BOTH a cut along a caller-supplied direction and a squeeze of surface cells toward the stamp centre, decided from ONE snapshot of the region taken before any write. Calling an erode and a pinch in sequence SHALL NOT be considered equivalent: the second would read cells the first had already moved, which produces a rounded ditch rather than a closed fold, and is the outcome the snapshot discipline exists to prevent everywhere else in this engine.

The cut direction SHALL be supplied by the caller, as it is for `flatten` and `scrape`. Occupancy carries no surface normal to average, so a verb that guessed one would be guessing.

The depth SHALL be a SIGNED parameter distinct from the brush strength: positive carves and negative raises a ridge. Strength is the falloff-and-dither weight every verb shares, so a negative strength means "no cells pass", not "the other direction" — the same reason `inflate` carries a signed `amount` of its own.

A zero depth SHALL touch no cell and record nothing into an open sculpt layer.

The verb SHALL respect a painted mask and a fractional strength by the same rules every other voxel verb follows, without per-verb code, and SHALL be recorded by an open sculpt layer so that the pass stays dialable afterwards.

Applying it SHALL be DETERMINISTIC and ORDER-INDEPENDENT: the same grid, parameters and seed SHALL produce the same cells on every run and platform, and the result SHALL NOT depend on the order in which cells are visited.

**The lattice limit SHALL be documented rather than left to be discovered.** Occupancy is binary, so a groove narrower than one cell cannot be represented and a crease on a coarse grid staircases. This is the exact analogue of the polygon-density requirement the same brush carries in a mesh tool, and the resolution level stack — refining where the detail goes — is the answer to it.

#### Scenario: A crease cuts a V
- **WHEN** a crease is stroked across a flat region
- **THEN** a groove is left whose floor is the requested depth below the surface, and which is narrower at the top than an erode-only cut of the same depth

#### Scenario: Inverted, it raises a ridge
- **WHEN** the depth is negative
- **THEN** a ridge is raised rather than a groove cut

#### Scenario: It is not the two verbs in sequence
- **WHEN** the same stroke is applied as a crease, and as an erode followed by a pinch
- **THEN** the two results differ

#### Scenario: A zero depth is free
- **WHEN** a crease is applied with a depth of zero
- **THEN** no cell changes and an open sculpt layer records nothing
