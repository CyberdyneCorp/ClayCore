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

### Requirement: Flatten samples a new volume rather than editing one
Flatten SHALL build its result by SAMPLING a source field with the plane blended in, so that the new band brackets the flattened surface. It SHALL NOT transform an existing volume's stored samples in place.

This is why: a narrow band tracks the surface only while the surface stays inside it. Smoothing moves the surface by less than a cell, so `relax` can rewrite samples where they lie. Flatten moves it by many band widths, and once the surface has walked outside the band there are no samples left describing where it now is — the isosurface comes apart. Sampling builds the band around the flattened surface instead, and makes the blend closed-form: no iteration, no step budget, no band to narrow afterwards.

Where an exact source exists — a document's field — flatten SHALL prefer it to a volume, because a volume reports a lower bound rather than a distance outside its own band, and sampling a field that mixes the two records the boundary between them as part of the shape.

#### Scenario: The band brackets the flattened surface
- **WHEN** a shape is flattened so its surface moves well beyond the original band
- **THEN** the result stores samples at the new surface, and none where the old one was

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a flattened volume
- **THEN** it stops at the facet rather than passing through it

### Requirement: The declared Lipschitz is measured, not assumed
A region blends under a weight that varies across it, which adds a term proportional to how far the value moves times the gradient of the weight — so flatten CAN make the field steeper than its source. The result SHALL declare a Lipschitz bound its samples actually satisfy, and that bound SHALL be measured from the samples produced rather than bounded in advance.

#### Scenario: The declared bound is not exceeded
- **WHEN** the steepest slope of a flattened volume is measured inside the sampled region
- **THEN** it does not exceed what the tape declares for that volume

#### Scenario: A tighter taper declares more
- **WHEN** the same flatten is applied with a narrow falloff and with a generous one
- **THEN** the narrow one declares the higher Lipschitz, and the document's safe step scale drops accordingly

### Requirement: Flatten acts where it is aimed, and a region is required
Flatten SHALL REQUIRE a region — a centre, a radius and a falloff. Outside it the field SHALL be unchanged, and the transition SHALL taper rather than step, so flattening does not leave a rim.

The region is not optional, because flatten is local by nature: where its weight is one the result IS the plane. Blending with no region at full strength therefore does not flatten a shape, it replaces it with a half-space — a ball comes back as a box. A request with no region SHALL be refused rather than honoured into destroying the shape.

#### Scenario: Outside the region nothing moves
- **WHEN** a shape is flattened with a region covering only part of it
- **THEN** the field away from that region is unchanged

#### Scenario: The region's edge does not leave a rim
- **WHEN** the change the flatten made is examined across the boundary of the region
- **THEN** it varies continuously rather than stepping

#### Scenario: A request with no region is refused
- **WHEN** flatten is asked for with a region radius of zero
- **THEN** it is refused, and the shape is not replaced by a half-space
