# sdf-kernels — sweeping along a guide

Delta for `add-swept-n`.

## ADDED Requirements

### Requirement: Profiles swept along a guide
The tape SHALL provide an opcode that carries a tessellated guide polyline and two or more 2D profiles, and evaluates a query point by finding the nearest point on the guide, expressing the point in a frame there, and evaluating the profiles interpolated along the guide's arc length.

The frame SHALL be **parallel-transported** along the guide rather than derived per-sample from the curve's own derivatives, so that it does not flip at an inflection point or become undefined where the guide is straight. Transport is sequential, so the frames SHALL be computed once when the item is compiled and interpolated between at evaluation.

Profiles SHALL be distributed by **arc length**, so a guide whose vertices bunch does not bunch the profiles.

#### Scenario: A sweep follows its guide
- **WHEN** a circle is swept along an L-shaped guide
- **THEN** material is present along both limbs and absent off them

#### Scenario: The cross-section is the profile
- **WHEN** a circle of a given radius is swept along a straight guide
- **THEN** between the guide's endpoints the field matches a capsule of that radius, within tolerance

#### Scenario: The ends are the profile, not a rounded cap
- **WHEN** a swept item is evaluated past the end of its guide, on the guide's axis
- **THEN** the distance is the overshoot past a flat end face, because the profile need not be a circle and there is no hemisphere to cap it with

#### Scenario: Profiles interpolate along the guide
- **WHEN** a wide profile and a narrow one are swept along a straight guide
- **THEN** the cross-section is wide at the start and narrow at the end

#### Scenario: The frame does not flip where the guide straightens
- **WHEN** a non-rotationally-symmetric profile is swept along a guide that bends, straightens, then bends back
- **THEN** the profile's orientation varies smoothly along the whole guide

#### Scenario: A degenerate sweep is refused
- **WHEN** a sweep is built with fewer than two guide points, or fewer than two profiles
- **THEN** it is refused

### Requirement: A sweep declares its exactness and curvature cost
A swept item SHALL declare itself **not exact**, and SHALL declare a Lipschitz factor derived from the guide's tightest bend against the widest profile's extent: a point at perpendicular offset `r` inside a bend of radius `R` is compressed by `R / (R - r)`.

Where the widest profile reaches or exceeds the tightest bend radius the sweep folds through itself. The engine SHALL NOT refuse this — a guide is editable after the fact — and SHALL instead report a correspondingly large Lipschitz, so the raymarcher takes small steps rather than stepping through a surface it was told was a distance field.

#### Scenario: A sweep is not exact
- **WHEN** a document containing a sweep is compiled
- **THEN** the tape reports the field as inexact

#### Scenario: A tighter guide steps more carefully
- **WHEN** the same profile is swept along a gently curved guide and a sharply curved one
- **THEN** the sharply curved one reports the smaller safe step scale

#### Scenario: An overgrown profile degrades rather than failing
- **WHEN** a profile wider than the guide's tightest bend radius is swept
- **THEN** the document still compiles and evaluates, and its safe step scale is very small
