# sdf-kernels — relaxing a surface

Delta for `add-sdf-relax`.

## ADDED Requirements

### Requirement: A sampled field can be relaxed
The library SHALL smooth a sampled volume, returning a new one whose surface is less bumpy than the one it was given. Repeated application SHALL smooth further, and smoothing SHALL be a no-op in shape terms on a surface that is already smooth.

#### Scenario: A bumpy surface becomes smoother
- **WHEN** a surface with small bumps on it is relaxed
- **THEN** the bumps are reduced, and the more iterations are run the less of them remains

#### Scenario: A smooth surface barely moves
- **WHEN** a sphere is relaxed
- **THEN** its surface stays where it was, to within the sampling

#### Scenario: Relaxing yields an ordinary item
- **WHEN** a relaxed volume is placed in a document
- **THEN** it combines, saves and evaluates exactly as any other volume does

### Requirement: Relaxing preserves the bound that sphere tracing depends on
Smoothing destroys exactness — the relaxed field no longer reports the true distance to its own surface — but it SHALL NOT break the Lipschitz bound. A weighted average of a field cannot vary faster than the field does, and a field whose slope is bounded by one is automatically a conservative bound on the distance to its own zero set.

A relaxed volume SHALL therefore still be safe to sphere trace: a ray marched at it SHALL arrive at the surface rather than step through it.

#### Scenario: The slope does not grow
- **WHEN** the steepest slope of a field is measured before and after relaxing
- **THEN** it has not increased

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a relaxed volume
- **THEN** it stops at the surface rather than passing through it

### Requirement: Relax acts where it is aimed
Relax SHALL accept a region — a centre, a radius and a falloff — so that it is a brush rather than only a global filter. Outside the region the field SHALL be unchanged, and at the edge of the region the change SHALL taper rather than step, so that relaxing does not leave a visible rim.

#### Scenario: Outside the region nothing moves
- **WHEN** a shape is relaxed with a region covering only part of it
- **THEN** the field away from that region is unchanged

#### Scenario: The region's edge does not leave a rim
- **WHEN** the field is examined across the boundary of the relaxed region
- **THEN** it varies continuously rather than stepping

### Requirement: Relax does not inflate a shape without limit
Smoothing shrinks convex features and grows concave ones. Repeated relaxing SHALL NOT cause a shape to grow without bound or to vanish after a moderate number of passes.

#### Scenario: Repeated relaxing converges rather than diverging
- **WHEN** a shape is relaxed many times over
- **THEN** its enclosed size settles rather than running away

### Requirement: An operator that transforms a field works on the samples
A field's value where it has no samples is a lower bound, not a measurement. An operator that reads a volume and writes another SHALL work on the stored samples rather than by re-sampling through evaluation, because evaluation mixes measurements with bounds and re-sampling that mixture records the boundary between them as though it were part of the shape.

An operator that MOVES the surface SHALL narrow the band by how far it moved, since the sample-free bricks were classified against where the surface used to be and their bounds would otherwise overstate the distance to where it is now.

#### Scenario: Smoothing does not manufacture a steep edge
- **WHEN** a volume is relaxed and the steepest slope of the result is measured within the sampled region
- **THEN** it has not risen above the slope of the field that went in

#### Scenario: A sample shared by several bricks is found in any of them
- **WHEN** a stored sample lying on a brick face, edge or corner is read by global coordinate
- **THEN** it is found whichever of the bricks sharing it holds the samples
