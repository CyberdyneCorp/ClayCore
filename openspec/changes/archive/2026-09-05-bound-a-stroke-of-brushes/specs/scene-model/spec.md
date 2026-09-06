# scene-model — what a chain of brushes is charged for

Delta for `bound-a-stroke-of-brushes`.

## ADDED Requirements

### Requirement: A chain of finite-support brushes is charged for what can meet

An item's declared Lipschitz bound SHALL be derived from the deformers that can
act together at one point, and SHALL NOT grow with the number of deformers that
cannot.

A brush with finite support is the identity outside its own region. Two such
brushes contribute a compounded factor only where both are non-identity for one
evaluation, which requires that a point inside the first can still be inside the
second when the second sees it. What carries it there is the travel of the links
BETWEEN them; the travel of the chain as a whole SHALL NOT be used, because it
grows with every brush added and so makes every pair look reachable on a
sufficiently worked model.

Reachability SHALL NOT be closed transitively for this purpose. Three brushes
where the first meets the second and the second meets the third, but the first
and third do not, have no point at which all three act — and a bound that
multiplies all three charges a compounding that cannot happen. Along a stroke
that is every brush on the model.

The result SHALL remain an upper bound: every deformer acting at a point
contains that point, so any set that acts together lies within the reach of each
of its members.

#### Scenario: A walked stroke does not compound along its length
- **WHEN** brushes are placed along a surface so that each overlaps only its neighbours
- **THEN** the bound does not grow in proportion to how many were placed

#### Scenario: Brushes on one spot still compound
- **WHEN** brushes are placed on top of one another
- **THEN** each additional one makes the bound larger, because they genuinely do stack

#### Scenario: The relaxed bound is still a bound
- **WHEN** the field of a chain of spread brushes is marched by the declared step
- **THEN** no step crosses the surface
