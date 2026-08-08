# sdf-kernels — the field verbs honour the mask

Delta for `add-mask-stroke-brush`.

## ADDED Requirements

### Requirement: Field verbs honour the mask
Every field verb SHALL accept an optional mask, and where one is given its per-sample weight SHALL be scaled by one minus the mask value at the WORLD position of the sample being written. A fully masked sample SHALL keep its source value exactly, not approximately: a frozen region that drifts by a rounding error is a frozen region that has moved.

This closes a real gap rather than adding a convenience. Relax and flatten rewrite or re-sample a field under a spherical region and a falloff, and nothing else gates them, so a masked region inside that sphere is not frozen — "a mask blocks any effect of sculpting" was false for exactly the verbs SDF layers gained most recently.

The mask SHALL be sampled in world units rather than in the volume's cells, which costs nothing — world addressing is what the mask field exists for — and is what lets one mask gate a voxel layer and an SDF layer at the same time.

#### Scenario: Relax leaves a frozen region alone
- **WHEN** a volume is relaxed over a region that is fully masked
- **THEN** the samples there are unchanged, while the same relax without the mask changes them

#### Scenario: Flatten leaves a frozen region alone
- **WHEN** a field is flattened over a region that is fully masked
- **THEN** the surface there is where the source put it, not on the plane

#### Scenario: Partial masking attenuates
- **WHEN** a field verb runs over a half-masked region
- **THEN** the surface there moves less than it does unmasked, and more than it does fully masked
