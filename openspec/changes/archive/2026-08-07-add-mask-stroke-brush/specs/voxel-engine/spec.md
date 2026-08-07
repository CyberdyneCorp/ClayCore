# voxel-engine — a bounded mask complement

Delta for `add-mask-stroke-brush`.

## ADDED Requirements

### Requirement: A mask can be inverted over a finite region
The mask SHALL support inverting over a caller-supplied region, and filling one with a constant. Over that region the result SHALL be `1 - value` for every cell in it, painted or not, and cells outside it SHALL be untouched. The boundary SHALL be the region the caller gave, never the storage's chunk grid.

Inverting a sparse unbounded lattice is not a representable operation, which is why `invert` is defined over what has been painted. That leaves the most common masking gesture — mask a region, invert, and edit everything else — unexpressible, because the complement a caller means is bounded by the model rather than by what has been touched.

`invert` SHALL keep its existing meaning: the two answer different questions, and a caller who worked around the unbounded one must not have it silently redefined.

#### Scenario: Everything else becomes masked
- **WHEN** a blob is painted, and the mask is inverted within a box containing it
- **THEN** the blob's cells read unmasked and the rest of the box reads fully masked

#### Scenario: The region's edge is the region's edge
- **WHEN** a mask is inverted within a box whose faces do not lie on the storage's chunk boundaries
- **THEN** the transition happens at the box's faces, and nothing outside the box changes

#### Scenario: Filling a region
- **WHEN** a region is filled with a value
- **THEN** every cell whose centre lies in it reads that value, and the field is empty again when the value is zero
