# sdf-kernels — masking that gates any operation

Delta for `add-masking-that-gates-any-op`.

## ADDED Requirements

### Requirement: A mask gates an item's participation
An item SHALL be able to carry a mask that scales how much it contributes at each point. Where the mask is fully protective the accumulated field SHALL be exactly what it was before the item; where the mask is absent the result SHALL be exactly the unmasked result.

This SHALL apply to every combine operation, including booleans, not only to edits made through the stroke engine.

#### Scenario: A fully protected region is untouched by a subtract
- **WHEN** a subtract item overlaps a region whose mask is fully protective
- **THEN** the field in that region is exactly what it was without the item

#### Scenario: An unprotected region is unaffected by the mask's presence
- **WHEN** an item is masked but the mask is zero over the evaluated region
- **THEN** the field is exactly the unmasked result

### Requirement: A gated item declares its Lipschitz cost
Mixing two fields by a spatially varying weight is not a distance field. The tape SHALL charge the mix against the item's Lipschitz bound, derived from the mask's own gradient bound, so raymarching a gated document does not overshoot.

#### Scenario: A uniform mask costs no step scale
- **WHEN** an item's mask is constant over its influence
- **THEN** the safe step scale is what the same item unmasked reports

#### Scenario: Raymarching a gated document does not overshoot
- **WHEN** a gated item's document is marched by its declared step scale
- **THEN** no step lands past the surface
