## ADDED Requirements

### Requirement: A regional level's normals and frames are the dense hierarchy's

A vertex a regional level stores SHALL hold the normal and the detail frame the
uniformly refined hierarchy holds at the same point on the surface, in the same
way its POSITION already holds that value bit for bit.

**A frame is not a shading detail here.** A multires surface reconstructs as
`P(n) = S(n) + Frame · Detail`, so a boundary frame off by an angle means a
coefficient authored there reconstructs to a different world-space offset than
the same coefficient on a dense hierarchy. The bit-identity a regional level
already guarantees otherwise holds only while boundary detail is zero — and with
detail zero there is nothing for the frame to be wrong about, which is why a gate
over an unsculpted fixture cannot see it.

The neighbourhood that completes the ring SHALL be the one the hierarchy already
derives, not a second derivation of it.

#### Scenario: A refined region's boundary normals match the dense hierarchy
- **WHEN** a region is refined to a level and compared corner-for-corner against a hierarchy refined everywhere to that level
- **THEN** the normals agree to within the measured summation-order noise at every corner, the region's boundary included

#### Scenario: Detail authored across a boundary reconstructs where it was authored
- **WHEN** a non-zero coefficient is authored at a region-boundary vertex and the surface is reconstructed
- **THEN** the world-space offset it produces is the offset the same coefficient produces on a dense hierarchy

#### Scenario: A uniform hierarchy is unchanged
- **WHEN** a hierarchy whose every level refines every patch is evaluated
- **THEN** its normals and frames are what they were, and no neighbourhood is built

### Requirement: The completed ring is summed in the level's own weighting

The faces beyond a level SHALL contribute to a vertex normal under the SAME
weighting as the faces the level stores.

The level's own sum is area-weighted by construction — its face normals are
unnormalized, and their magnitude is twice the projected area. A neighbourhood
contribution that normalized each face and weighted it by corner angle would
weight a boundary vertex differently from an interior one, producing a seam that
is visible and unattributable.

**This is observable wherever the surface is CURVED, and nowhere else.** A planar
cage agrees under both weightings exactly, however unequal its face areas are, so
unequal areas are not the discriminating property and a fixture chosen for them
can be blind.

The tolerance a gate on this uses SHALL sit between the measured summation-order
noise on a correct implementation and the measured error of the wrong weighting,
and BOTH numbers SHALL be stated. Exact equality SHALL NOT be required: the two
hierarchies sum the same faces in different orders, float addition is not
associative, and a gate demanding exact agreement would fail a correct
implementation.

#### Scenario: The wrong weighting is caught
- **WHEN** the neighbourhood's contribution is summed under a different weighting from the level's own
- **THEN** the boundary normals differ from the dense hierarchy's by orders of magnitude more than the summation-order noise, and the gate fails

#### Scenario: A planar fixture cannot decide it
- **WHEN** the two weightings are compared on a cage with no curvature
- **THEN** they agree exactly, whatever the grading, so such a fixture proves nothing about the choice

### Requirement: The neighbourhood can be released without releasing the level

A host SHALL be able to release the cross-level neighbourhoods alone, leaving
every level resident, evaluated, and bit-identical in its positions and normals.

It is the cheapest thing in this class to give back and among the more expensive
to hold: it is derived from the level below by the stencils that built the level,
so it rebuilds exactly, and it is re-read on every access regardless. Every other
release in this class costs a level its evaluation.

A release SHALL NOT advance the cache generation. Nothing a bound sculptor holds
a reference into moves, and advancing it would rebind every live sculptor to
announce a change it cannot observe.

**The cost SHALL be stated where the call is declared**: the next access rebuilds
the neighbourhood rather than walking its rim, so a release taken mid-stroke is
paid by the next dab.

#### Scenario: A release gives back the neighbourhood and nothing else
- **WHEN** the neighbourhoods are released
- **THEN** the bytes given back are at least what their arrays hold, the memory report falls by at least that much, and every level's positions and normals are bit-identical afterwards

#### Scenario: A released neighbourhood is rebuilt, not read as absent
- **WHEN** a level whose neighbourhood was released is re-evaluated
- **THEN** it is rebuilt, and the level's normals are what an untrimmed hierarchy produces

#### Scenario: A level that never had one is distinguishable
- **WHEN** a level stores every child of every face of its parent
- **THEN** it has no neighbourhood, and that is decided from its topology rather than inferred from the absence of one
