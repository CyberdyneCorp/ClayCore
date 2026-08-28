# scene-model — the chain pad grows with the chain

Delta for `narrow-the-chain-pad`.

## MODIFIED Requirements

### Requirement: A hard blend drags no chain, and so pads no cull region
The pad a per-brick cull region takes for a smooth-union chain SHALL be the largest single-item DRAG in the layer, and a HARD profile drags nothing: its smin is a step, so the running value it produces is a plain minimum and moves by no blend radius at all. A blend radius left on a hard node SHALL contribute nothing to that pad.

`Paint` and the extended modes SHALL keep the drag they have. Paint's colour fades over `max(support, k)` whatever the profile, and the extended modes ignore the profile by design and measure their own support.

This SHALL NOT be applied to a node's OWN bound, which keeps its `max(support, k)` dilation. In a mixed chain that dilation is margin for the drag a node's smooth neighbours apply to a running value it contributed to, and removing it measured 540 to 10,105 band-clamped disagreements against the full tape on a 12-node hard/smooth document — against 540 to 540, exactly, for narrowing the pad alone.

The narrowing SHALL NOT move any value: band-clamped results stay identical to the full tape over a document mixing hard and smooth combines in one chain, sampled widely enough that a change in what the cull drops is visible.

#### Scenario: A hard node sets no pad
- **GIVEN** a layer whose every node carries a hard profile and a non-zero blend radius
- **WHEN** the cull pad for that layer is computed
- **THEN** it is zero — and the same layer blended smoothly pads by `min(support, k * envelope(N))`, that profile's support clamped down by the chain-pad envelope at the layer's effective contributor count, so the two are distinguishable

#### Scenario: The largest drag is the smooth one
- **GIVEN** a chain alternating a hard node with the layer's LARGEST blend radius and smooth nodes with a smaller one
- **WHEN** the cull pad is computed
- **THEN** it is the smooth drag, not the hard node's radius

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

## ADDED Requirements

### Requirement: The chain pad grows with the chain it compiles, clamped at support
A smooth distance blend's contribution to the layer's cull pad SHALL be `min(support, k * envelope(N))`: a measured per-profile envelope in k-multiples that rises with `N`, clamped at the profile's support so the pad never exceeds the pre-envelope `max(support, k)` anywhere.

`N` SHALL be the layer's EFFECTIVE contributor count: the node-map size times the symmetry multiplicity `1 + popcount(mirror_axes) + max(0, radial_count - 1)` — the compiler emits a mirrored item once per mirror and radial copy, each copy a real contributor to the layer's one serial chain, and the modes compose additively. Counting the map alone under-pads an amplified chain and is a refuted proxy.

The SEAM blends the symmetry copies enter through — the layer's mirror and radial blend radii, independent of any item radius — SHALL fold into the pad as a quadratic chain term of the largest seam radius, clamped at the ceiling the item blends alone resolve to (the pre-envelope pad). Where the seam demands more than that ceiling, the pad SHALL equal the pre-envelope pad exactly.

The pad SHALL resolve at read time from raw per-profile maxima and live counts, so that an incrementally appended cull index resolves exactly what a fresh build reports, and every input only rises under an append. A symmetry edit is not an append and rebuilds the index.

The envelope is evidence-bound: it holds at least the knees' seed-draw drift above every measured knee, and it SHALL NOT be lowered without a sweep of comparable breadth. The correctness bar is relative — equal-or-fewer band-clamped in-band disagreements than the pre-envelope pad, per config — because no fixed dilation is a proof for an arbitrary chain.

#### Scenario: The pad rises with the chain
- **GIVEN** two layers blending at the same radius and profile, one map of 75 nodes and one of 1200
- **WHEN** their cull pads are computed
- **THEN** the longer chain pads wider, and neither pad exceeds the profile's support

#### Scenario: A symmetric layer pads by the chain it compiles
- **GIVEN** a 75-node layer under radial count 64, its items participating in the symmetry
- **WHEN** the cull pad is computed
- **THEN** the envelope resolves at ~4800 effective contributors, not 75 — and per-brick culled values stay band-clamp identical to the full tape where resolving at 75 measured real disagreements

#### Scenario: A seam wider than any item blend still pads
- **GIVEN** items blending at radius k under a layer seam radius of 3k
- **WHEN** the cull pad is computed
- **THEN** it equals the ceiling the item blends resolve to — the pre-envelope pad — never wider, and the culled values stay band-clamp identical to the full tape

#### Scenario: Never wider than the pre-envelope pad
- **GIVEN** any layer, symmetric or not, seams or not
- **WHEN** the cull pad is computed
- **THEN** it is at most the pre-envelope `max(support, k)` pad, and where the clamp binds the culled tape is bit-identical to that pad's compile

#### Scenario: An appended index resolves what a fresh build reports
- **GIVEN** a cull index built before a stroke and extended by its appended dabs
- **WHEN** the pad is resolved after the append
- **THEN** it equals the pad a fresh build of the grown document reports, exactly
