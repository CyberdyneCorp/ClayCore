# scene-model

## ADDED Requirements

### Requirement: A layer carries a radial symmetry mode
A layer SHALL carry a radial symmetry described by a count, an axis and a seam blend. A count of 0 or 1 SHALL mean the mode is off and SHALL cost nothing at evaluation.

When the count is 2 or more, every participating item in the layer SHALL evaluate as itself plus `count - 1` copies, each rotated about the layer-local axis by `2πk / count` for `k` in `1 .. count-1`. The copies SHALL be combined into the item's own value, so a radial layer presents one accumulated field rather than N independent items.

The axis SHALL pass through the origin of the layer's local frame, so the layer transform moves it and it persists in the document. Clearing the count SHALL restore the un-arrayed field exactly.

#### Scenario: A single item becomes an N-fold rosette
- **WHEN** a layer with one off-axis sphere is given a radial count of 6 about Y
- **THEN** the field is invariant under rotation by 60° about the layer's local Y axis, and sampling at the sphere's centre rotated by any multiple of 60° returns the same distance

#### Scenario: Turning it off restores the field
- **WHEN** a radial count is set and then cleared
- **THEN** the document evaluates identically to a probe of the same points taken before the count was set

#### Scenario: The axis follows the layer
- **WHEN** the layer holding a radial array is translated
- **THEN** the array's centre moves with it, because the axis is the layer-local one rather than a world axis

### Requirement: Radial symmetry uses the mirror's participation rule
An item SHALL participate in its layer's radial symmetry under the same flag that governs its participation in the layer mirror. An item excluded from the layer mirror SHALL also be excluded from the radial array, so a single asymmetric detail is excluded once rather than twice.

A stroke SHALL participate, because a stroke resolves into ordinary items — which is the property that makes this a sculpting mode rather than a modelling one.

#### Scenario: An excluded item does not repeat
- **WHEN** an item on a radial layer has its mirror participation cleared
- **THEN** that item appears once while every other item in the layer appears `count` times

#### Scenario: A stroke on a radial layer repeats
- **WHEN** a stroke is applied to a layer with a radial count of 4
- **THEN** the stamps it resolved into each appear 4 times, without the caller touching the resolved nodes

### Requirement: The radial seam blends like the mirror seam
The radial seam blend SHALL follow the semantics of the mirror blend: 0 SHALL be a hard union between a copy and its neighbours, and a positive value SHALL smooth-weld them where they meet. A positive seam SHALL mark the layer's tape as smooth-blended for exactness tracking, exactly as the mirror seam does.

#### Scenario: A positive seam welds neighbouring copies
- **WHEN** two adjacent copies of an item overlap and the seam blend is positive
- **THEN** the surface between them is welded rather than creased, and the layer reports a smooth blend rather than an exact field

### Requirement: Radial and mirror compose additively
When both the radial count and one or more mirror axes are active, each SHALL contribute its own copies of the base item. The change SHALL NOT emit the products of the two — a rotated reflection is not emitted — which matches the existing mirror, where enabling two axes emits one reflection per axis rather than the four of a full two-plane symmetry.

#### Scenario: Both modes active emits both sets
- **WHEN** a layer has a radial count of 3 and mirroring on X
- **THEN** each participating item evaluates as itself, plus 2 rotated copies, plus 1 reflected copy, and not as the 6 of a combined group

### Requirement: A radial layer reports the influence it actually occupies
An item's influence bound on a radial layer SHALL cover every copy the mode emits, so culling and the brick cache do not drop a copy that is on screen.

#### Scenario: A culled region still sees the far copies
- **WHEN** a brick overlapping only the far side of a radial array is evaluated
- **THEN** the item whose copy reaches that brick is compiled into the brick's tape
