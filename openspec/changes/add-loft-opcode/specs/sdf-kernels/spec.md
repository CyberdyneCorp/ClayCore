# sdf-kernels — the loft opcode

Delta for `add-loft-opcode`.

## MODIFIED Requirements

### Requirement: Lifts of 2D profiles
`lift.h` SHALL provide exact extrusion and exact revolution of exact 2D profiles (01 §2.6), and a loft flagged as bound.

Extrusion and revolution SHALL be expressible in the tape as primitive opcodes carrying a profile, so a document can build profile-driven shapes. **Loft SHALL also be expressible in the tape**, as an opcode carrying two or more profiles interpolated along the lift axis; the header SHALL expose the interpolation as a function taking an already-computed parameter, so that bracketing among more than two profiles needs no second entry point.

#### Scenario: An extrusion is exact
- **WHEN** an extruded exact profile is evaluated
- **THEN** the field is an exact distance

#### Scenario: A loft reaches both profiles
- **WHEN** a loft between two different profiles is evaluated at each end of its depth
- **THEN** the cross-section there matches the profile at that end

#### Scenario: A loft interpolates between them
- **WHEN** a loft is evaluated halfway along its depth
- **THEN** the cross-section lies between the two profiles rather than matching either

#### Scenario: More than two profiles are bracketed
- **WHEN** a loft carries three profiles and is evaluated at the middle one's position
- **THEN** the cross-section matches that middle profile

### Requirement: Field-info tracking
Every primitive, combine and deformer SHALL declare whether it yields an exact distance and what Lipschitz factor it introduces, and the compiler SHALL fold those into the tape so the safe step scale is correct.

A loft SHALL declare itself **not exact**, and SHALL declare a Lipschitz factor accounting for the interpolation along the lift axis — a lerp of two fields adds a term proportional to how far apart they are over the depth they are interpolated across. Declaring Lipschitz 1 for a loft SHALL be treated as a defect, because the raymarcher would overstep and miss surfaces.

#### Scenario: A loft is not exact
- **WHEN** a document containing a loft is compiled
- **THEN** the tape reports the field as inexact

#### Scenario: Differing profiles over a short depth step more carefully
- **WHEN** two lofts differ only in that one interpolates between very different profiles over a shallow depth
- **THEN** that one reports the smaller safe step scale
