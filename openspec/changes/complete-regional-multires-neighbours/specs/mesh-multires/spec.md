## ADDED Requirements

### Requirement: A regional level's normals and frames are the dense hierarchy's

A vertex a regional level stores SHALL hold the normal and the detail frame the
uniformly refined hierarchy holds at the same point on the surface, to
tolerance, in the same way its POSITION already holds that value bit for bit.

**A frame is not a shading detail here.** A multires surface reconstructs as
`P(n) = S(n) + Frame · Detail`, so a boundary frame that is off by an angle
means a coefficient authored at that vertex reconstructs to a different
world-space offset than the same coefficient on a dense hierarchy. The
bit-identity a regional level already guarantees therefore holds only while
boundary detail is zero — and that is the only case a gate written over an
unsculpted fixture exercises.

The measured defect this replaces: normals wrong by up to 0.406 (23.4 degrees)
at level 1, 0.209 at level 2 and 0.103 at level 3, at exactly the vertices whose
face ring at that level is incomplete, with positions identical to 0.000000000
at every level.

#### Scenario: A refined region's boundary normals match the dense hierarchy
- **WHEN** a region of a hierarchy is refined to a level and compared corner-for-corner against a hierarchy refined everywhere to that level
- **THEN** the normals agree to tolerance at every corner, including the region's boundary

#### Scenario: Detail authored across a boundary reconstructs where it was authored
- **WHEN** a non-zero detail coefficient is authored at a region-boundary vertex and the surface is reconstructed
- **THEN** the world-space offset it produces is the offset the same coefficient produces on a dense hierarchy, to tolerance

#### Scenario: A uniform hierarchy is unchanged
- **WHEN** a hierarchy every one of whose levels refines every patch is evaluated
- **THEN** its normals and frames are what they were, and no neighbourhood beyond its own stored faces is built

### Requirement: The cross-level neighbourhood is one object, not a rule per consumer

The topologically correct ring at a vertex — including where adjacent base
patches are stored at different refinement depths — SHALL be produced once and
consumed by every operation that needs a ring, rather than each of normals,
frames, smoothing and relaxation carrying its own transition rule.

It SHALL be derived from the same subdivision the level itself is built by, so
that a neighbour beyond the stored region holds what the dense level holds at
that point for the same reason a stored vertex does, rather than by a separate
approximation that has to be argued about.

It SHALL be REBUILDABLE and released under memory pressure with the rest of a
level's runtime cache, and it SHALL cost the region's BOUNDARY rather than its
area: a neighbouring face belongs to it only when it touches a vertex the level
stores.

#### Scenario: The neighbourhood is released and rebuilt identically
- **WHEN** a hierarchy's caches are dropped and a level is evaluated again
- **THEN** the neighbourhood is rebuilt and the normals and frames it produces are identical

#### Scenario: The neighbourhood costs the boundary
- **WHEN** a region is refined and its neighbourhood is built
- **THEN** it holds only faces touching a stored vertex, and grows with the region's boundary rather than with the surface outside it
