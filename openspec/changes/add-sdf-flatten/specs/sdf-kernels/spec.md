# sdf-kernels — flattening a surface

Delta for `add-sdf-flatten`.

## ADDED Requirements

### Requirement: A sampled field can be flattened onto a plane
The library SHALL pull a sampled volume's surface toward a caller-supplied plane, returning a new volume. Where the effect is at full weight the surface SHALL become that plane.

Flattening SHALL mean the same thing it means for voxels: material on the plane's positive side is removed AND hollows on the negative side that touch material are filled. It is two-sided, not a subtract. Two representations sharing a verb's name must share its meaning, or a document means something different depending on which one it is stored in.

#### Scenario: A bump becomes a facet
- **WHEN** a shape with a raised bump is flattened against a plane cutting through it
- **THEN** the surface under the brush lies on that plane

#### Scenario: A dent is filled, not deepened
- **WHEN** a shape with a hollow below the plane is flattened
- **THEN** the hollow is filled up to the plane rather than left or cut deeper

#### Scenario: A surface already on the plane does not move
- **WHEN** a flat face that already lies on the target plane is flattened
- **THEN** it stays where it was, to within the sampling

#### Scenario: Flattening yields an ordinary item
- **WHEN** a flattened volume is placed in a document
- **THEN** it combines, saves and evaluates exactly as any other volume does

### Requirement: A bounded step keeps the field traceable
A single pass SHALL move a sample's value by no more than a stated step, and repeated passes SHALL be how a surface travels further. The falloff SHALL be widened where it is too narrow for the step it was given, rather than obeyed into producing a field that cannot be traced. The resulting field SHALL declare a Lipschitz bound it actually satisfies.

This is why: blending a field toward a plane under a weight that varies across a region adds a term proportional to how far the value moves times the gradient of the weight. Left unbounded that makes the field steeper than it declares, and a raymarcher steps through a field steeper than its declared bound. Bounding the per-pass movement caps that term however far the surface ultimately travels.

#### Scenario: The declared bound is not exceeded
- **WHEN** the steepest slope of a flattened volume is measured inside the sampled region
- **THEN** it does not exceed what the tape declares for that volume

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a flattened volume
- **THEN** it stops at the surface rather than passing through it

#### Scenario: More passes travel further
- **WHEN** the same shape is flattened with one pass and with several
- **THEN** the surface is closer to the plane after several, and one pass moves it by no more than the step

#### Scenario: A narrow falloff is widened rather than obeyed
- **WHEN** flatten is asked for a falloff too narrow for the step it was given
- **THEN** the falloff used is widened, and the resulting field still satisfies its declared bound

### Requirement: Flatten acts where it is aimed
Flatten SHALL accept a region — a centre, a radius and a falloff — so that it puts a facet on part of a shape rather than reshaping all of it. Outside the region the field SHALL be unchanged, and the transition SHALL taper rather than step, so flattening does not leave a rim.

#### Scenario: Outside the region nothing moves
- **WHEN** a shape is flattened with a region covering only part of it
- **THEN** the field away from that region is unchanged

#### Scenario: The region's edge does not leave a rim
- **WHEN** the change the flatten made is examined across the boundary of the region
- **THEN** it varies continuously rather than stepping
