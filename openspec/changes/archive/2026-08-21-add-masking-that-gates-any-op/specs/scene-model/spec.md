# scene-model — masking that gates any operation

Delta for `add-masking-that-gates-any-op`.

## ADDED Requirements

### Requirement: A gated item is a document concept that survives the file
An item's mask SHALL be part of the document: it SHALL serialize, reload and evaluate identically, and a document with no gated item SHALL serialize to exactly the bytes it does today.

#### Scenario: A document with no gated item is unchanged
- **WHEN** a document containing no masked item is serialized
- **THEN** the bytes are identical to those the previous format version produced

### Requirement: Gating does not widen an item's influence
A mask SHALL only reduce where an item acts. An item's influence bound SHALL therefore remain valid when the item is gated, so per-brick culling needs no change.

#### Scenario: A gated item culls exactly as its ungated form does
- **WHEN** the cull plan is built for a document whose item is gated
- **THEN** the bricks selected are the same as for the ungated item, and none that the gated item affects are skipped
