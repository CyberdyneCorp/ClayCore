# scene-model — a stroke extends the cull index rather than rebuilding it

Delta for `append-the-cull-index`.

## ADDED Requirements

### Requirement: An appended item extends the cull index
The cull index SHALL be extendable for a document that has gained items at the tail of its last visible SDF layer's root list, without recomputing the bounds of the items already in it. A stroke appends one item per stamp and every stamp invalidates the index, so rebuilding it walks the whole document to add one item: 2.45 ms at 50,000 items, of which 2.29 ms is bounds that did not move.

An extension SHALL produce the index a fresh build produces — the same chains with the same cached bounds and the same prunability, and the same cull pad — so that the per-brick tapes compiled through it are byte-identical to those compiled through a rebuilt one. Chain ORDER need not match, because a plan keys chains by layer and child list and those keys are unique.

An extension SHALL reach EVERY chain over that root list, not only the last layer's. An instanced layer shares its content with the layer it instances, so one root list is compiled once per layer that names it, and each is its own chain with its own bounds because an item's geometry bound reads the layer's transform and mirror. Extending one and not the others leaves them describing a document that no longer exists, which is a silently smaller tape rather than a failure.

An extension SHALL be REFUSED, leaving the index unchanged, wherever it cannot be certain the document changed only in that way. A refusal costs the rebuild the caller would have paid anyway; a wrong extension is silent.

The cull pad SHALL be re-derived rather than carried, since both of its terms are maxima over a layer's visible nodes and an appended item can raise either.

#### Scenario: An extended index is a rebuilt index
- **GIVEN** a document whose last items were appended to a chain
- **WHEN** per-brick tapes are compiled through an index extended by that append and through one built fresh
- **THEN** the tapes are byte-identical, and the two indexes report the same cull pad

#### Scenario: An instanced layer's second chain is extended too
- **WHEN** an item is appended to a root list that two layers compile
- **THEN** both layers' chains carry it, each with the bound its own layer's transform gives

#### Scenario: A stroke's cost does not follow the document
- **WHEN** the same dab is appended to indexes over documents of very different sizes
- **THEN** what the extension costs is set by the appended item rather than by how much precedes it

#### Scenario: An uncertain append is refused
- **WHEN** the items claimed as appended are not the tail of the layer they name, or there are more of them than it holds
- **THEN** the index is left exactly as it was and the caller rebuilds
