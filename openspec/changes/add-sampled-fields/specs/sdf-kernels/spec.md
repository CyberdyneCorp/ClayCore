# sdf-kernels — sampled fields

Delta for `add-sampled-fields`.

## ADDED Requirements

### Requirement: A field can be built from samples
The library SHALL provide a sparse narrow-band signed distance volume built by sampling any callable over a region, storing samples only within a stated band of the surface. Bricks wholly outside the band SHALL store no samples, recording only whether they are inside or outside, so that storage is proportional to surface area rather than to volume.

The volume SHALL be expressible in the tape as a primitive opcode, so a sampled field combines with every op the engine already has.

#### Scenario: A sampled sphere reproduces its source
- **WHEN** a sphere's field is sampled into a volume and evaluated near the surface
- **THEN** the values match the sphere's own field within the sampling tolerance

#### Scenario: Storage follows the surface, not the volume
- **WHEN** a volume is built over a region much larger than the surface it contains
- **THEN** its stored size is far smaller than a dense grid over the same region

#### Scenario: Far from the band the sign is still right
- **WHEN** a volume is evaluated deep inside and far outside its surface
- **THEN** the values are negative and positive respectively

#### Scenario: A volume combines like any primitive
- **WHEN** a sampled volume is subtracted from a box
- **THEN** the result is the box with that shape removed

### Requirement: A sampled field declares what it is
A sampled volume SHALL declare itself **not exact**. Its guarantees divide by whether a point lands where the volume kept samples — which is **not** the same as being within the band, since a brick is kept whole and so holds samples well beyond it.

Where the volume **has** samples, the value is an interpolation. Interpolating a convex field overshoots: trilinear interpolation lies above the function it samples by an amount proportional to the square of the cell size. It SHALL NOT be claimed as a lower bound there. What it SHALL be is accurate to the sampling, with the error shrinking as the cell size does, so that choosing a cell size is a real control over accuracy rather than a hope.

Where it has **none**, the value SHALL be a true lower bound — never larger in magnitude than the real distance — so that sphere tracing cannot overstep across the empty majority of the region.

The declared Lipschitz factor SHALL account for the interpolant being able to be steeper than the field it samples, rather than assuming the source's own factor carries over.

#### Scenario: A volume is not exact
- **WHEN** a document containing a sampled volume is compiled
- **THEN** the tape reports the field as inexact

#### Scenario: Where there are no samples the value is a lower bound
- **WHEN** a volume is evaluated at points that fall in bricks holding no samples, inside and outside the surface
- **THEN** the reported distance never exceeds the true distance in magnitude

#### Scenario: The interpolant's slope is declared, not assumed
- **WHEN** a document containing a sampled volume is compiled
- **THEN** the reported Lipschitz factor exceeds 1, and the document's safe step scale drops accordingly

#### Scenario: Where there are samples the error follows the cell size
- **WHEN** the same surface is sampled at a coarse and at a fine cell size
- **THEN** the worst error where samples are stored is small at both and markedly smaller at the fine one

#### Scenario: A skipped brick meets a stored one safely
- **WHEN** the field is evaluated on both sides of the boundary between a brick that stores samples and one that does not
- **THEN** the value jumps, and each side is separately either a lower bound or accurate to the sampling, so a marcher crossing it cannot overstep

### Requirement: The bound has to be usable, not merely true
A lower bound that is correct but tiny stops a raymarcher as surely as a wrong one: it takes steps that never grow, and runs out of iterations before it arrives. The bound reported where there are no samples SHALL therefore **grow with distance from the surface** rather than being a constant.

The value reported outside the sampled region SHALL NOT fall to zero at the region's edge. The distance to the sampled box alone does, and a sphere tracer reads zero as a surface — every ray would stop on an invisible shell where the sampling happened to stop.

#### Scenario: Empty space opens up as it empties
- **WHEN** a volume is evaluated at increasing distances from the surface, through space holding no samples
- **THEN** the reported distance keeps increasing, and exceeds a single brick well before the region's edge

#### Scenario: Crossing empty space takes a sane number of steps
- **WHEN** a sphere trace crosses the empty part of a sampled region
- **THEN** it arrives in a number of steps proportional to the distance over the growing bound, not to the distance over the band width

#### Scenario: A ray finds the surface, not the edge of the sampling
- **WHEN** a ray is marched at a sampled volume from outside its region
- **THEN** it stops at the real surface, and a ray that misses the surface passes through the region without stopping
