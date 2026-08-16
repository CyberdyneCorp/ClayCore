# sdf-kernels — alphas on SDF layers

Delta for `add-sdf-alphas`.

## ADDED Requirements

### Requirement: An alpha stamp displaces a surface under finite support
An SDF item SHALL accept a caller-supplied 2D scalar stamp applied as a distance offset under a bounded region, projected along a caller-supplied direction, with the same radial falloff the grab and magnify deformers use.

The engine SHALL NOT decode images. The caller SHALL supply `width * height` samples in [0, 1].

Outside the region the field SHALL be untouched, so a stamp is a local edit rather than a whole-item one.

#### Scenario: A stamp of all zeros is the identity
- **WHEN** an alpha whose samples are all zero is applied to an item
- **THEN** the field is identical to the item without it, exactly

#### Scenario: Material outside the region is untouched
- **WHEN** a point outside the stamp's radius is evaluated
- **THEN** the value is exactly the undeformed item's

#### Scenario: A stamp displaces along its direction
- **WHEN** a stamp with a non-zero amplitude is applied to a surface facing its direction
- **THEN** the surface moves by the amplitude scaled by the stamp value and the falloff at that point

### Requirement: An alpha's Lipschitz bound comes from its steepness
The tape SHALL derive an alpha's Lipschitz contribution from the largest difference between adjacent samples over the texel size, together with the falloff's own gradient — NOT from the largest sample value, which is flat when constant and would bound a stamp that displaces nothing as though it were the steepest possible.

#### Scenario: A flat stamp contributes no steepness of its own
- **WHEN** two alphas with the same peak value and amplitude but different sample steepness are applied
- **THEN** the flat one's safe step scale is higher, and a flat stamp's step scale does not change with its resolution

#### Scenario: A steep stamp costs step scale honestly
- **WHEN** an alpha with a large adjacent-sample difference is applied
- **THEN** the safe step scale falls, and raymarching the result produces no overshoot

#### Scenario: The same relief spread wider is less steep
- **WHEN** the same stamp is applied over a larger extent
- **THEN** the safe step scale rises, because the bound is a world-space slope rather than a sample difference
