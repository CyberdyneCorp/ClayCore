# sdf-kernels — cull the bake's tape per brick, exactly

Delta for `cull-the-bake-tape`.

## ADDED Requirements

### Requirement: A bake evaluates only the items a brick can see
Sampling a document into a volume SHALL evaluate, for each brick, only the items whose influence can reach it, rather than the whole document at every sample. A brick of an ordinary sculpt needs a handful of a tape thousands of instructions long, and the cost of a bake is the interpreter rather than the arithmetic.

The result SHALL be identical to evaluating the whole tape everywhere — sample for sample, not within a tolerance. That is available without approximation because a culled value can only exceed the true one, so a culled value inside the band IS the true one, and a brick with no sample inside the band stores none of them and is read only for which side it is on.

Where a brick DOES store samples, the ones beyond the band SHALL be evaluated against the whole tape. They are stored as they are, and a culled value there is too large — a volume that overstates the distance to its own surface is one a sphere tracer steps through, which is the single thing the sparse representation may not do. Those samples SHALL be evaluated in batches rather than one at a time, since they are a minority of the lattice and scattered across it.

Culling SHALL be REFUSED where it does not pay. A blend's cull pad grows with its radius, so a wide enough blend leaves every item in every brick's tape and the per-brick compile becomes pure overhead — measured, that is a bake at half the speed of not culling at all. Whether it pays SHALL be decided by measuring the culled tapes of a sample of the WHOLE lattice, not by a rule over the document's parameters, and not from a sample of one region of it.

A verb that transforms the sampled block before it is stored SHALL NOT cull, because the classification that makes culling exact is about the values the fill produced and not about the values that end up stored.

#### Scenario: A culled bake is the whole-tape bake
- **GIVEN** a document whose items are spread over its surface
- **WHEN** it is baked with the tape culled per brick, and again with the whole tape
- **THEN** the two volumes have the same bricks, declare the same Lipschitz bound, and serialize to the same bytes

#### Scenario: The culled bake does not overstate its own distance
- **WHEN** the field of a culled bake is compared against the true distance at points outside the surface
- **THEN** it exceeds it by no more than the whole-tape bake does

#### Scenario: A wide blend refuses the cull rather than paying for it
- **GIVEN** a document blended widely enough that a brick's tape is most of the document's
- **WHEN** it is baked
- **THEN** the bake is no slower than evaluating the whole tape, and the volume is the same

#### Scenario: A single item is baked without ceremony
- **GIVEN** a document of one primitive
- **WHEN** it is baked
- **THEN** the volume is the one the whole tape produces
