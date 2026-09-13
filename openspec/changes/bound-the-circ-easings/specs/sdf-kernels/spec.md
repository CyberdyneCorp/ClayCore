## ADDED Requirements

### Requirement: An easing curve used as a falloff has a bounded slope

An easing curve that a finite-support deformer uses as its falloff SHALL have a
bounded derivative over the domain that deformer evaluates it on, and the slope
declared for it SHALL be an upper bound on that derivative.

A curve whose derivative is unbounded has no Lipschitz constant, so no declared
slope can bound it. Establishing one by sampling is not merely loose there but
wrong in the unsafe direction: the declared value comes back finite and too
small, the step scale derived from it is too large, and the marcher steps past
the surface. That is wrong geometry rather than slow geometry, and it is
silent.

Where such a curve is offered, it SHALL be held short of its singularity so that
its supremum exists and is expressible in closed form, and the declared slope
SHALL be computed from the same constant that holds it, so the curve and its
bound cannot diverge.

**Holding the curve SHALL NOT break its continuity.** A curve assembled from
two halves must still join where they meet; a discontinuity introduced while
bounding a slope is a worse defect than the one being fixed. The held curve
SHALL therefore still reach its endpoints exactly.

#### Scenario: The declared slope bounds the curve
- **WHEN** a curve whose derivative is unbounded is sampled densely and compared against its declared slope
- **THEN** no observed slope exceeds what is declared

#### Scenario: The curve still joins itself
- **WHEN** a curve assembled from two halves is evaluated either side of the join
- **THEN** the two values agree

#### Scenario: The endpoints are exact
- **WHEN** the curve is evaluated at the ends of its domain
- **THEN** it reaches them exactly, so a falloff still applies its full weight at full strength
