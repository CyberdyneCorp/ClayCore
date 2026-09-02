# scene-model — extending the cull index costs the appended subtree

Delta for `extend-the-index-without-the-document`.

## MODIFIED Requirements

### Requirement: An appended item extends the cull index
The cull index SHALL be extendable for a document that has gained items at the tail of its last visible SDF layer's root list, without recomputing the bounds of the items already in it. A stroke appends one item per stamp and every stamp invalidates the index, so rebuilding it walks the whole document to add one item: 2.45 ms at 50,000 items, of which 2.29 ms is bounds that did not move.

An extension SHALL produce the index a fresh build produces — the same chains with the same cached bounds and the same prunability, and the same cull pad — so that the per-brick tapes compiled through it are byte-identical to those compiled through a rebuilt one. Chain ORDER need not match, because a plan keys chains by layer and child list and those keys are unique.

An extension SHALL reach EVERY chain over that root list, not only the last layer's. An instanced layer shares its content with the layer it instances, so one root list is compiled once per layer that names it, and each is its own chain with its own bounds because an item's geometry bound reads the layer's transform and mirror. Extending one and not the others leaves them describing a document that no longer exists, which is a silently smaller tape rather than a failure.

An extension SHALL be REFUSED, leaving the index unchanged, wherever it cannot be certain the document changed only in that way. A refusal costs the rebuild the caller would have paid anyway; a wrong extension is silent.

The cull pad SHALL be raised from the appended subtree rather than recomputed from the document, and SHALL be kept as its two terms PER LAYER. The document's pad is a maximum over layers of the sum of that layer's two maxima; folding the terms across layers instead would store a sum of maxima, which is larger — so safe to cull with — but no longer the number a fresh build reports. Per layer the two are the same number, which is what makes raising them exact.

The subtree an extension reads SHALL be every node it reaches, including the children of a group the build does not descend into, because the pad folds over the layer's flat node map and an invisible group's visible child sets it either way.

An extension SHALL be REFUSED when the touched layer's node map did not grow by exactly the subtree named. The pad's terms are raised from that subtree alone, so a map that gained anything else would leave them below what a fresh build reports, and a pad that is too small plans against too small a region — the one direction that loses items a brick needed.

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

#### Scenario: The pad an append raises is a maximum of sums
- **GIVEN** one layer whose pad is all feather and another whose pad is all blend
- **WHEN** an item is appended to the second
- **THEN** the document's pad is the larger of the two layers' sums, not the sum of the larger of each term

#### Scenario: A widening blend inside an appended group raises the pad
- **WHEN** the appended node is a group, or an invisible group, whose child blends wider than anything already in the layer
- **THEN** the extended index reports the pad a fresh build reports

#### Scenario: An uncertain append is refused
- **WHEN** the items claimed as appended are not the tail of the layer they name, or there are more of them than it holds
- **THEN** the index is left exactly as it was and the caller rebuilds

#### Scenario: An append the node map does not corroborate is refused
- **WHEN** the layer's node map gained a node outside the subtree the append names
- **THEN** the extension is refused rather than left with a pad below the one a rebuild would report

## ADDED Requirements

### Requirement: A cached cull index is extended in place only while unobserved
A cached cull index MAY be extended in place instead of being copied, and SHALL be copied whenever any other holder of it exists. The copy protects a reader holding the index against a plan it already made; a reader takes its handle under the same lock the extension runs under and holds it while it reads, so the absence of any other handle, observed under that lock, is what makes extending in place safe.

#### Scenario: A stroke extends the cached index without copying it
- **WHEN** consecutive appends are absorbed by a document whose cull index nothing else is holding
- **THEN** no copy of the index is made, and every read gets the index a rebuild would give

#### Scenario: A held index is not mutated under its holder
- **GIVEN** a caller holding the cull index
- **WHEN** an append is absorbed
- **THEN** the caller's index is unchanged and the cache takes a copy to extend
