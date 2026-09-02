# scene-model — a bound read through a node's ancestors

Delta for `bound-an-edit-by-the-node-it-names`.

## MODIFIED Requirements

### Requirement: Influence bounds
Every edit item and group SHALL expose a conservative influence bound: its shape AABB dilated by blend radius and rounding. The bound SHALL be conservative in the narrow-band sense that all evaluated storage relies on: outside the bound (dilated by the band width), band-clamped field values are unaffected by the item. (Raw far-field values may legitimately shift when a smooth-blend operand changes — smin deviates wherever |a−b| is inside the support width — which is why the guarantee, like brick storage, is stated band-clamped.)

When an item carries deformers, its bound SHALL additionally account for the domain warp before transform and dilation: rotational warps (twist, bend) SHALL widen the bound to the axis-aligned hull of the shape's rotational sweep, cross-section scaling (taper) SHALL scale by the largest factor in its range, and displacement SHALL dilate by its amplitude.

A node held inside one or more groups SHALL additionally expose the bound it reaches THROUGH those groups: its own bound dilated, at each enclosing group in turn, by that group's blend support. This is the conservative answer in the same band-clamped sense as the bound above — outside it, band-clamped values are unaffected by an edit to that node — and it SHALL be the answer given wherever a caller asks where an edit to that node lands. A group's ancestry SHALL contribute its blend support and nothing else: the group's other children are geometry the edit cannot reach, and SHALL NOT widen the answer.

Where the enclosing subtree combines non-locally, the ancestor walk SHALL report the unbounded state rather than a finite box, on the same terms as the influence bound of any non-local node.

#### Scenario: Bound is conservative
- **WHEN** a property test samples the field with and without an item at points outside the item's influence bound dilated by a band width β, clamping values to ±β
- **THEN** the two clamped fields are bit-identical at every sampled point

#### Scenario: Deformed item stays inside its bound
- **WHEN** the same property test runs on items carrying twist, bend, taper, and displacement deformers
- **THEN** the clamped fields remain bit-identical outside the widened bound, and per-brick culled tapes over those scenes stay band-clamp identical to the full tape


#### Scenario: A node inside a group reaches past its own box
- **WHEN** the conservativeness property test runs on a child of a smooth-blended group, sampling outside the child's own bound but inside the group's blend support
- **THEN** the band-clamped fields differ there, so the child's own bound alone is NOT the answer to where an edit to it lands

#### Scenario: The ancestor-path bound is conservative
- **WHEN** the same property test samples outside the child's bound dilated by every enclosing group's blend support
- **THEN** the two band-clamped fields are bit-identical at every sampled point

#### Scenario: A far sibling is not part of the answer
- **GIVEN** a group holding one small child and a large one far from it
- **WHEN** the ancestor-path bound of the small child is taken
- **THEN** it is strictly smaller than the group's influence bound and does not contain the far sibling's geometry
