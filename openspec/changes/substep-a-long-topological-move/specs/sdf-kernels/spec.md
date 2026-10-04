## ADDED Requirements

### Requirement: A topological move longer than its reach does not fold
A topological move SHALL NOT pull two output points from the same source point because its displacement outruns its reach. When `|displacement| · ease_max_slope(ease) / radius` exceeds one half, the move SHALL run as the fewest equal slices that keep each slice at or below that bound, up to 64 slices. Each slice SHALL be anchored where the slices before it left the grip and SHALL solve its geodesic over the material those slices left. The source SHALL still be read once per output sample, at the composed pull-back.

A drag at or below the bound SHALL be a single slice, identical to the unsliced move. The number of slices SHALL be available to callers. A drag needing more than 64 slices runs with longer ones and is not promised to stay one-to-one.

#### Scenario: A drag over twice the reach rises along its length
- **WHEN** a unit sphere's crown is dragged by (0.5, 0, 0.4) from (0, 0, 1) at radius 0.3 on a linear curve
- **THEN** the surface height at the anchor is at least 1.0, and the height increases monotonically along x from 0 to 0.35, within 0.04 of the same drag made as nine calls of one ninth

#### Scenario: The slices are what separate calls would give
- **WHEN** the same drag is made as one call and as n separate calls of d/n, each anchored where the last left the grip, with n the call's own slice count
- **THEN** the two surfaces agree within 0.01 along the drag

#### Scenario: A short drag is one slice
- **WHEN** a drag's displacement is under half the radius on a linear curve
- **THEN** it is one slice
