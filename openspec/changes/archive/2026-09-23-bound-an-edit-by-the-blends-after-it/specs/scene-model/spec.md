## MODIFIED Requirements

### Requirement: Influence bounds
Every edit item and group SHALL expose a conservative influence bound: its shape AABB dilated by blend radius and rounding. The bound SHALL be conservative in the narrow-band sense that all evaluated storage relies on: outside the bound (dilated by the band width), band-clamped field values are unaffected by the item. (Raw far-field values may legitimately shift when a smooth-blend operand changes — smin deviates wherever |a−b| is inside the support width — which is why the guarantee, like brick storage, is stated band-clamped.)

When an item carries deformers, its bound SHALL additionally account for the domain warp before transform and dilation: rotational warps (twist, bend) SHALL widen the bound to the axis-aligned hull of the shape's rotational sweep, cross-section scaling (taper) SHALL scale by the largest factor in its range, and displacement SHALL dilate by its amplitude.

A node held inside one or more groups SHALL additionally expose the bound it reaches THROUGH those groups: its own bound dilated, at each enclosing group in turn, by that group's blend support. This is the conservative answer in the same band-clamped sense as the bound above — outside it, band-clamped values are unaffected by an edit to that node — and it SHALL be the answer given wherever a caller asks where an edit to that node lands. A group's ancestry SHALL contribute its blend support and nothing else: the group's other children are geometry the edit cannot reach, and SHALL NOT widen the answer.

That answer SHALL also carry, at every level — the node's own chain, then each enclosing group's — the reach of the SMOOTH combines that FOLLOW it in that chain, taken before the group's own support: the widest of those combines' full blend supports (and a feathered replace's band, and a symmetry seam's support, on the terms the chain pad counts them). A node is the running value of its chain wherever it is the nearest thing, so an edit changes that value far from the node's own box; a hard union leaves that beyond-band difference alone, while a smooth combine further down reads the running value out to its support and carries it back into the band. A later sibling GROUP SHALL contribute its own combine only — its children start a chain of their own and never read the running value. A node with only hard combines after it SHALL NOT widen, so a node appended last, and every document without a smooth blend, keeps exactly the bound it had.

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

#### Scenario: A smooth sibling after a node carries its edit past the node's own box
- **GIVEN** two spheres in one chain, the first hard and the second blended smooth after it, at the layer root or inside a group whose own combine does not apply
- **WHEN** the conservativeness property test moves the first sphere and samples outside its own bound dilated by the band
- **THEN** the band-clamped fields differ there, and outside its ancestor-path bound, which carries the second sphere's blend support, they are bit-identical

#### Scenario: Only a later smooth combine widens the answer
- **WHEN** the ancestor-path bound is taken of a node followed only by hard combines, or of the last node of a chain
- **THEN** it is the node's own bound, and a smooth sibling after the node widens it by exactly that sibling's blend support

#### Scenario: An undone grab clamped into the node's bound still covers the sibling's fillet
- **GIVEN** a grab at the head of the first node's chain whose ball lies beyond the node's band but on the surface of a smooth sibling after it
- **WHEN** the grab is undone and redone and a brick cache refills only the reported bounds
- **THEN** it matches a cache rebuilt from nothing, in both directions
