# sdf-kernels — the loft opcode

Delta for `add-loft-opcode`.

## MODIFIED Requirements

### Requirement: Lifts
`lift.h` SHALL provide exact extrusion and exact revolution of exact 2D profiles (01 §2.6), and a loft flagged as bound.

Extrusion and revolution SHALL be expressible in the tape as primitive opcodes carrying a profile, so a document can build profile-driven shapes. **Loft SHALL also be expressible in the tape**, as an opcode carrying two or more profiles interpolated along the lift axis. The header SHALL expose the interpolation as a function taking an already-computed parameter, so bracketing among more than two profiles needs no second entry point — a signature that derived the parameter from the height could only ever serve exactly two.

#### Scenario: Revolve preserves exactness
- **WHEN** an exact 2D profile is revolved
- **THEN** the resulting 3D field is exact and the tree exactness state records `exact`

#### Scenario: Lifted items evaluate through the tape
- **WHEN** a circle profile is extruded and, separately, revolved in a document
- **THEN** the fields equal a capped cylinder and a torus respectively, within meshing tolerance

#### Scenario: Lifted items keep tracked exactness
- **WHEN** a tape containing only extrusions and revolutions of exact profiles is compiled
- **THEN** its field info remains exact and its safe step scale stays 1

#### Scenario: A loft reaches both profiles
- **WHEN** a loft between two different profiles is evaluated at each end of its depth
- **THEN** the cross-section there matches the profile at that end

#### Scenario: A loft interpolates between them
- **WHEN** a loft is evaluated halfway along its depth
- **THEN** the cross-section lies between the two profiles rather than matching either

#### Scenario: More than two profiles are bracketed
- **WHEN** a loft carries three profiles and is evaluated at the middle one's position
- **THEN** the cross-section matches that middle profile, rather than a blend of the outer two

### Requirement: Exactness and Lipschitz propagation
The library SHALL track per-node field classification — `exact`, `bound`, or `Lipschitz(L)` — through the expression tree (01 §2.7) and expose the resulting safe step scale for the composed field. No consumer (sphere tracing, meshing, brick fill) SHALL assume |∇f| = 1 unless the tree proves exactness.

A loft SHALL declare itself **not exact**, and SHALL declare a Lipschitz factor accounting for the interpolation along the lift axis: a lerp of two fields adds a term proportional to how far apart they are over the depth they are mixed across, steepened by the easing curve's own maximum slope. Declaring Lipschitz 1 for a loft SHALL be treated as a defect, because the raymarcher would step as though the field were a distance and miss surfaces.

#### Scenario: Composition downgrades correctly
- **WHEN** an exact primitive is wrapped in a taper (Lipschitz L) and then smooth-unioned with an exact sphere
- **THEN** the composed tree reports the conservative classification and a safe step scale ≤ 1/L

#### Scenario: A loft is not exact
- **WHEN** a document containing a loft is compiled
- **THEN** the tape reports the field as inexact

#### Scenario: Differing profiles over a shorter depth step more carefully
- **WHEN** two lofts differ only in that one interpolates between the same profiles over a shallower depth
- **THEN** that one reports the smaller safe step scale
