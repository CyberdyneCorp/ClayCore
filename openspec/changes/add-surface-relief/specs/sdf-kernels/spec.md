# sdf-kernels — surface relief

Delta for `add-surface-relief`.

## ADDED Requirements

### Requirement: An item can displace the surface accumulated before it
The library SHALL provide a combine op that offsets the ACCUMULATED field by an amplitude, weighted by the item's own field used as a region. The item SHALL contribute its shape as a region rather than as geometry, in the way the paint op already uses an item as a region for colour.

Offsetting a distance field moves its isosurface along the field's own gradient, which is the surface normal, so this displaces the existing surface along its normal rather than approximating that.

A SIGNED amplitude SHALL cover both directions — building the surface up and cutting it in — because they are one operation, as magnify and pinch are.

#### Scenario: A positive amplitude builds the surface up
- **WHEN** a relief item with a positive amplitude overlaps an existing surface
- **THEN** the surface within the region moves outward along its normal

#### Scenario: A negative amplitude cuts into it
- **WHEN** the same item is given a negative amplitude
- **THEN** the surface within the region moves inward

#### Scenario: It acts on what came before, not on itself
- **WHEN** a relief item is placed in a layer with no other content
- **THEN** it contributes no surface of its own

#### Scenario: The region is any primitive
- **WHEN** relief items using different primitives as their region are applied
- **THEN** each displaces the surface over the footprint of its own primitive

### Requirement: Relief has finite support
Outside the item's region the accumulated field SHALL be exactly unchanged, so that item influence bounds stay tight and brick culling keeps working — which is what makes relief usable at the densities a stroke produces.

#### Scenario: Beyond the region nothing moves
- **WHEN** the field is evaluated beyond a relief item's falloff
- **THEN** it is identical to the field without that item

#### Scenario: A zero amplitude changes nothing
- **WHEN** a relief item is given an amplitude of zero
- **THEN** the field is unchanged everywhere

### Requirement: The steepness relief adds is declared
Offsetting the accumulated field by a weighted amplitude raises its slope by that term's gradient, which is the amplitude over the falloff width. The tape's Lipschitz SHALL carry it, so a deep relief through a narrow falloff costs the marcher by a declared amount rather than by surprise.

#### Scenario: The field stops being exact
- **WHEN** a document containing a relief item is compiled
- **THEN** it reports the field as inexact and the safe step scale is below one

#### Scenario: A deeper or narrower relief declares more
- **WHEN** the amplitude is raised, or the falloff width narrowed
- **THEN** the reported Lipschitz rises and the safe step scale falls

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a surface carrying relief
- **THEN** it stops at the displaced surface rather than passing through it
