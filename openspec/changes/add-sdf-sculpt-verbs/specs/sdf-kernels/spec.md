# sdf-kernels — sculpting verbs for an SDF layer

Delta for `add-sdf-sculpt-verbs`.

## ADDED Requirements

### Requirement: Region-weighted field shaping
The kernel SHALL provide operations that shape the ACCUMULATED field inside a bounded region, rather than combining a new volume into it. Each SHALL be weighted by the region so its effect falls to zero at the boundary and the field stays continuous.

These are distinct from RELIEF and INCISE, which offset the surface along its own normal by a fixed amplitude. A shaping verb changes the surface's SHAPE within the region — averaging it, swelling it, drawing it toward a plane — which an offset cannot express.

They are also distinct from deformers, which warp a single item's input space and travel with that item. A shaping verb applies to whatever field is present at that place, including the seam between two items where neither owns the surface.

#### Scenario: The effect falls to zero at the region boundary
- **WHEN** any shaping verb is applied over a region
- **THEN** the field outside the region is unchanged, and the field is continuous across the boundary

#### Scenario: A verb over empty space changes nothing
- **WHEN** a shaping verb's region contains no surface
- **THEN** the field is unchanged

#### Scenario: Smoothing a seam neither item owns
- **WHEN** two items are unioned with a small blend radius, leaving a visible crease, and smoothing is applied over the crease
- **THEN** the crease is softened without changing either item and without altering the blend radius

### Requirement: A shaping verb declares its exactness cost
Any verb that samples the accumulated field at more than one point SHALL declare itself **not exact** and SHALL contribute a Lipschitz factor, so the raymarcher takes smaller steps inside the region rather than stepping through a surface it was told was a distance field.

The evaluation cost multiplier inside the region SHALL be stated, since it is paid on every sample there.

#### Scenario: Smoothing lowers the safe step scale
- **WHEN** a document containing a smoothing verb reports its safe step scale
- **THEN** the scale is lower than the same document without it, and sphere tracing through the smoothed region does not overshoot the surface
