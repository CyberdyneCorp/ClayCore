# sdf-kernels — twist and bend take a span

Delta for `ranged-twist-and-bend`.

## ADDED Requirements

### Requirement: Ranged twist and bend
The kernel dialect SHALL provide twist and bend variants whose rotation is RAMPED across a caller-given span with an easing curve and HELD beyond it, so material outside the span travels rigidly rather than continuing to rotate.

Each SHALL be the same rotation as its unranged form with the angle substituted, NOT a second formulation. With a linear ease and a span covering the content, the ranged form SHALL equal the unranged form at every point inside the span, and this SHALL be asserted rather than assumed — it is what makes the pair a range on an existing deformation rather than a second deformation to keep in step.

A zero-width span SHALL be refused at the bindings rather than divided by.

The declared Lipschitz factor SHALL be charged the angular rate the easing curve actually REACHES, not its average across the span: an eased ramp is steeper somewhere in the middle than linear, and a bound taken from the average would be under the field's true slope exactly where the ramp is steepest.

The influence bound SHALL reuse the unranged hull, which contains the ranged warp because a bounded rotation about an axis is contained by the cylinder the unbounded one sweeps.

Both SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene with a NON-LINEAR ease, so a backend that applied the range but ignored the ease fails rather than passes.

#### Scenario: A ranged twist over its whole span is the unranged twist
- **WHEN** a ranged twist with a linear ease and a span covering the content is compared to the unranged twist at the same rate
- **THEN** every point inside the span warps identically

#### Scenario: Outside the span the rotation holds
- **WHEN** two points above the span differ only in height
- **THEN** they rotate by the same angle, where under the unranged twist they would not

#### Scenario: The ease is charged to the bound
- **WHEN** the same ranged twist is declared with a linear ease and with a steeper one
- **THEN** the steeper curve reports a tighter safe step scale, and both bound the field's measured slope

#### Scenario: A zero-width span is refused
- **WHEN** a caller passes a span whose ends are equal
- **THEN** the binding refuses it rather than dividing by zero
