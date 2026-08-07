# sdf-kernels — surface relief

Delta for `add-surface-relief`.

## ADDED Requirements

### Requirement: An item can displace the surface accumulated before it
The library SHALL provide a combine op that offsets the ACCUMULATED field by an amplitude, weighted by the item's own field used as a region. The item SHALL contribute its shape as a region rather than as geometry, in the way the paint op already uses an item as a region for colour.

Offsetting a distance field moves its isosurface along the field's own gradient, which is the surface normal, so this displaces the existing surface along its normal rather than approximating that.

Building the surface up and cutting into it SHALL be TWO ops sharing one implementation, rather than one op with a signed amplitude. The amplitude rides on `blend_k`, which is required non-negative in three places including the blend constructor — which has no op to be aware of — so a sign has nowhere to live. It is also the existing convention: add and subtract are a pair, and so are engrave and emboss.

Sharing the implementation is what keeps the two each other's inverse as either changes.

#### Scenario: Relief builds the surface up
- **WHEN** a relief item overlaps an existing surface
- **THEN** the surface within the region moves outward along its normal, by the amplitude

#### Scenario: Incise cuts into it
- **WHEN** the same item is given the incise op instead
- **THEN** the surface moves inward by the same amount

#### Scenario: It acts on what came before, not on itself
- **WHEN** a relief item is placed in a layer with no other content
- **THEN** it contributes no surface of its own

#### Scenario: The region is any primitive
- **WHEN** relief items using different primitives as their region are applied
- **THEN** each displaces the surface over the footprint of its own primitive

### Requirement: Relief has finite support
Outside the item's region the accumulated field SHALL be exactly unchanged, so that item influence bounds stay tight and brick culling keeps working — which is what makes relief usable at the densities a stroke produces.

The reach is the region's own extent PLUS its rounding PLUS the falloff width. The rounding does double duty here — it is the falloff width and it also rounds the region's own field, exactly as it does for groove and tongue, where the channel is centred on the rounded surface. The influence bound SHALL be dilated by both terms, not by the falloff alone.

#### Scenario: Beyond the region nothing moves
- **WHEN** the field is evaluated beyond a relief item's falloff
- **THEN** it is identical to the field without that item

#### Scenario: The bound covers the rounded region, not the raw one
- **WHEN** the furthest point a relief item changes is measured
- **THEN** it lies within the region's extent plus its rounding plus the falloff width, and within the item's declared influence bound

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

#### Scenario: The declared bound is one the field meets
- **WHEN** the steepest slope of a field carrying relief is measured
- **THEN** it does not exceed what the tape declares

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a surface carrying relief
- **THEN** it stops at the displaced surface rather than passing through it
