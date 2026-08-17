# sdf-kernels — surface colour

Delta for `decide-surface-colour`.

## ADDED Requirements

### Requirement: Painting colours a surface without moving it
A colour-only combine SHALL stain the accumulated field's colour and leave its distance untouched, so a paint stroke is not a shape edit.

#### Scenario: A paint stroke does not move the surface
- **WHEN** a stroke is applied to a layer with the paint op
- **THEN** the field's distance at every point is identical to before the stroke

#### Scenario: A painted point reports the authored colour
- **WHEN** the colour is read at a point a paint stamp covers
- **THEN** it is the colour the caller authored, not a blend with the item beneath

### Requirement: Consolidation preserves colour exactly
Baking a layer into a sampled volume SHALL carry every painted colour into the volume's per-sample colour. Consolidation is advertised as changing COST rather than appearance, and colour is part of appearance.

This is what lets colour resolution move from item-bound to texel-bound without an artist repainting.

#### Scenario: Colour survives a bake
- **WHEN** a layer carrying painted colour is consolidated
- **THEN** the colour read at any point is unchanged
