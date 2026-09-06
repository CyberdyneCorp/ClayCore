# brush-engine — a drag prepares once and resolves per frame

Delta for `add-sdf-sculpt-transaction`.

## ADDED Requirements

### Requirement: A drag resolves in two halves
Resolving a world drag into per-item warps SHALL be separable into the half that does not depend on how far the drag has gone and the half that does.

A drag holds its anchor and its radius fixed for the whole gesture and grows only its displacement. Everything that follows from the anchor and the radius — which items the drag reaches, where its centre lands in each item's own frame, what its radius becomes there — SHALL therefore be resolvable ONCE, and turning that into a warp for a given displacement SHALL then cost no scene access at all. Without the split, a live drag walks the whole edit list once per pointer event to rediscover an answer that cannot have changed.

The two halves composed SHALL be BIT-identical to resolving the drag in one step, for every item, for every displacement, including under a transformed layer, a rotated item, a per-axis scale, a non-default falloff, the front-only gate and items nested in groups. A preview whose commit differs in the last bits is a preview of something else, so the prepared form SHALL keep the terms the one-step form divides and rotates by rather than pre-inverted equivalents — a reciprocal multiplied is not a division.

Preparation SHALL be able to report what it walked — the nodes visited and, of those, the items the drag can reach — so that the property "the traversal is paid once" is testable as a counter rather than as a duration.

The refusals SHALL be unchanged by the split: a non-positive radius and a layer with no edit list SHALL prepare nothing, and an item the drag cannot reach SHALL receive no warp.

#### Scenario: Prepared and resolved is the same warp
- **WHEN** a drag is prepared once and resolved for each of several displacements
- **THEN** each resolved warp names the same node and carries the same deformer, bit for bit, as resolving that drag in one step

#### Scenario: The traversal happens once
- **GIVEN** a layer holding thousands of items of which a drag reaches two
- **WHEN** the drag is prepared and then resolved
- **THEN** preparation reports having visited every node and reached two, and resolving visits neither

#### Scenario: A radius that is not a drag prepares nothing
- **WHEN** a drag is prepared with a non-positive radius
- **THEN** nothing is prepared

### Requirement: The chain ordering rule applies to a chain a caller holds
The rule that places a drag's warp at the FRONT of an item's chain, and replaces a leading warp from the same drag rather than stacking another beside it, SHALL be expressible against a deformer chain held by value as well as against a node in the document.

A live gesture must hold the pre-stroke chain by value, because the node in the document is the one thing it has promised not to touch. Both forms SHALL be the same rule — the node form SHALL be the chain form applied to that node's chain — so a caller cannot get the ordering subtly wrong by holding its own copy.

#### Scenario: The two forms agree
- **WHEN** the ordering rule is applied to a node and to a copy of that node's chain, with the same warp
- **THEN** the two chains are identical deformer for deformer

#### Scenario: The coalescing rule travels with it
- **WHEN** the rule is applied twice with the same drag's warp to a chain held by value
- **THEN** the chain does not grow the second time
