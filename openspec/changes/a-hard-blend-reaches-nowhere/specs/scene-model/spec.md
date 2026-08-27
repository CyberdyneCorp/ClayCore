# scene-model — how far a combine drags a chain

Delta for `a-hard-blend-reaches-nowhere`.

## ADDED Requirements

### Requirement: A hard blend drags no chain, and so pads no cull region
The pad a per-brick cull region takes for a smooth-union chain SHALL be the largest single-item DRAG in the layer, and a HARD profile drags nothing: its smin is a step, so the running value it produces is a plain minimum and moves by no blend radius at all. A blend radius left on a hard node SHALL contribute nothing to that pad.

`Paint` and the extended modes SHALL keep the drag they have. Paint's colour fades over `max(support, k)` whatever the profile, and the extended modes ignore the profile by design and measure their own support.

This SHALL NOT be applied to a node's OWN bound, which keeps its `max(support, k)` dilation. In a mixed chain that dilation is margin for the drag a node's smooth neighbours apply to a running value it contributed to, and removing it measured 540 to 10,105 band-clamped disagreements against the full tape on a 12-node hard/smooth document — against 540 to 540, exactly, for narrowing the pad alone.

The narrowing SHALL NOT move any value: band-clamped results stay identical to the full tape over a document mixing hard and smooth combines in one chain, sampled widely enough that a change in what the cull drops is visible.

#### Scenario: A hard node sets no pad
- **GIVEN** a layer whose every node carries a hard profile and a non-zero blend radius
- **WHEN** the cull pad for that layer is computed
- **THEN** it is zero — and the same layer blended smoothly pads by that profile's support, so the two are distinguishable

#### Scenario: The largest drag is the smooth one
- **GIVEN** a chain alternating a hard node with the layer's LARGEST blend radius and smooth nodes with a smaller one
- **WHEN** the cull pad is computed
- **THEN** it is the smooth support, not the hard node's radius

#### Scenario: A node's own bound is unchanged
- **GIVEN** a node with a hard profile and a non-zero blend radius
- **WHEN** its geometric bound is read
- **THEN** it is still dilated by that radius, because a mixed chain needs the margin

#### Scenario: Paint and the extended modes keep what they drag
- **WHEN** the pad is computed for a layer holding a hard-profile Paint or an extended mode with a blend radius
- **THEN** that radius still contributes, because the colour fade and the channel both read it

#### Scenario: The field is unchanged
- **GIVEN** a document mixing hard and smooth combines in one chain, the hard ones carrying the larger blend radius
- **WHEN** a per-brick culled tape is evaluated against the whole-document tape inside the brick
- **THEN** every band-clamped value is equal exactly, over a sweep wide enough that the narrowed cull is exercised
