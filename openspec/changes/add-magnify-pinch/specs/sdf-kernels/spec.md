# sdf-kernels — magnify and pinch

Delta for `add-magnify-pinch`.

## ADDED Requirements

### Requirement: A radial scale about a point, with finite support
The library SHALL provide a deformer that scales space radially about a centre within a stated radius, so that the surface swells away from that centre or gathers toward it. ONE signed strength SHALL cover both directions: magnify and pinch are the same deformation with opposite sign, and giving them separate opcodes would be building the same thing twice.

Support SHALL be finite: outside the radius the field SHALL be exactly unchanged, so that item influence bounds stay tight and brick culling keeps working.

#### Scenario: A positive strength swells the surface
- **WHEN** a shape is magnified about a point on it
- **THEN** the surface near that point moves outward from the centre

#### Scenario: A negative strength gathers it
- **WHEN** the same deformer is applied with the sign reversed
- **THEN** the surface near that point moves toward the centre

#### Scenario: Support is finite
- **WHEN** the field is evaluated beyond the deformer's radius
- **THEN** it is identical to the field without the deformer

#### Scenario: Zero strength changes nothing
- **WHEN** the deformer is applied with a strength of zero
- **THEN** the field is unchanged everywhere

### Requirement: The stretch a radial scale costs is declared
Scaling space radially is not distance preserving, so the field is no longer exact and its slope grows. The tape's Lipschitz factor SHALL carry that, as it does for grab and pose, and the stretch SHALL account for the easing curve's slope because the deformation is steepest where the falloff is.

A raymarcher SHALL therefore still land on a magnified or pinched surface rather than stepping through it.

#### Scenario: The field is no longer exact
- **WHEN** a document containing a magnify deformer is compiled
- **THEN** it reports the field as inexact, and the safe step scale is below one

#### Scenario: A stronger deformation declares more
- **WHEN** the same shape is deformed at increasing strength
- **THEN** the reported Lipschitz rises and the safe step scale falls

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a magnified shape
- **THEN** it stops at the surface rather than passing through it
