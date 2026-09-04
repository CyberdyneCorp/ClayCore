# scene-model — what a rigid layer placement guarantees

Delta for `drag-a-layer-without-a-refill`.

## ADDED Requirements

### Requirement: A layer placement carries a classification
A layer's transform SHALL be classified by what it does to the layer's field, and the classification SHALL be part of what the scene model reports about a placement rather than something a caller re-derives:

- **RIGID** — a rotation and a translation, unit scale.
- **SIMILARITY** — a rotation, a translation and a uniform positive scale.
- **GENERAL** — anything else, which in this scene model means a per-axis layer scale.

RIGID SHALL be the subset of SIMILARITY whose scale factor is exactly 1, so a caller that handles similarity handles both.

The classification SHALL be made on the CHANGE from one placement to another rather than on either placement alone: a layer already carrying a uniform scale of 2 that goes to 3 has moved by a similarity of 1.5, and asking whether a placement "is" a similarity answers about the wrong thing.

A SIMILARITY SHALL additionally require that every distance term in the layer scales with the layer. It does not in general: a layer's uniform scale multiplies an item's ROUNDING and does not multiply its BLEND RADIUS. A layer holding a smooth combine with a non-zero radius therefore SHALL classify a scale change as GENERAL, because its field after the scale is not the field before it multiplied by anything.

#### Scenario: A translate and a rotate are rigid
- **WHEN** a layer is placed with a translation, a rotation, or both, at unit scale
- **THEN** the placement classifies RIGID

#### Scenario: A uniform scale is a similarity
- **WHEN** a layer is placed with a uniform positive scale beside any rotation and translation
- **THEN** the placement classifies SIMILARITY and reports the scale factor

#### Scenario: A per-axis scale is general
- **WHEN** a layer carries a scale whose components differ
- **THEN** the placement classifies GENERAL

#### Scenario: A scale on a blending layer is general
- **WHEN** a layer whose items carry a smooth combine with a non-zero radius is placed with a uniform scale
- **THEN** the placement classifies GENERAL, and the same layer moved rigidly still classifies RIGID

### Requirement: A similarity placement moves a layer's field and nothing else
For a layer whose placement classifies RIGID or SIMILARITY, re-placing that layer SHALL change its contribution to the document only by that placement. Writing `M` for the matrix taking the old placement to the new one and `s` for its uniform scale factor:

- the layer's field afterwards SHALL equal its field beforehand composed with `M⁻¹` and multiplied by `s`, in exact arithmetic. Implementations compose the placement into each item's transform, which ROUNDS, so a consumer comparing two evaluations SHALL expect agreement to within that rounding rather than bit equality — except where the composition happens to be exact, where the agreement SHALL be bitwise;
- the layer's surface afterwards SHALL be its surface beforehand mapped through `M`;
- no other layer's contribution SHALL change, because layers combine by hard union at the document level.

This SHALL hold for the SDF representation and is what makes a preview drawn under `M` exact for that layer's own surface rather than an approximation of it. It SHALL NOT be claimed for a GENERAL placement: a per-axis scale changes the field's Lipschitz behaviour, exactly as a per-axis item scale already does.

What the guarantee does NOT cover is the mutual occlusion of the hard union while the moved layer overlaps another: two surfaces each exactly placed still interpenetrate where the union would have resolved them. A consumer relying on this SHALL be told that limit.

#### Scenario: A rigid placement moves the surface exactly
- **WHEN** an SDF layer is meshed, then placed with a rotation and a translation, then meshed again
- **THEN** the second mesh equals the first mapped through the placement matrix, to the tolerance the mesher's own lattice alignment allows, and the field values agree at points mapped through the matrix — bitwise where composing the placement into the items' transforms is exact, and otherwise to within that composition's rounding

#### Scenario: A uniform scale scales distances
- **WHEN** an SDF layer is placed with a uniform scale factor `s`
- **THEN** the field at a point equals `s` times the previous field at that point mapped back through the placement

#### Scenario: Another layer is untouched
- **WHEN** one SDF layer of a multi-layer document is re-placed rigidly
- **THEN** the other layers' fields are bit-identical at every sampled point, and the document's field differs only where the moved layer's contribution wins the hard union

#### Scenario: A per-axis scale claims nothing
- **WHEN** a layer is placed with a per-axis scale
- **THEN** the placement classifies GENERAL and the field guarantee above is not asserted for it
