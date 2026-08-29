# scene-model — a topology gesture in the history

Delta for `add-dynamic-topology`.

## ADDED Requirements

### Requirement: The undo history reverses a topology gesture
The one undo history SHALL reverse a gesture that changed a surface's connectivity, alongside the scene commands, voxel passes, mask edits, surface-group edits and vertex deltas it already reverses.

The step SHALL carry a sparse topology delta rather than a snapshot, and a gesture SHALL be one step however many stamps and topology operations it contained.

The history SHALL reach the surface through a RESOLVER supplied by the owner above it, as the existing kinds do, because the module layering forbids `scene` from naming `mesh` at all.

A compound step SHALL be able to span a scene command and a topology gesture, so a crossing that creates a layer and sculpts into it undoes as one.

A journal SHALL encode, decode and replay the new kind, and a journal written before this change SHALL still replay.

#### Scenario: An adaptive stroke undoes as one step
- **WHEN** a stroke that split, collapsed and flipped edges is undone
- **THEN** the surface is bit-identical to before the stroke and one step was consumed

#### Scenario: An older journal still replays
- **WHEN** a journal recorded before this change is replayed
- **THEN** it applies exactly as it did before, and no step is skipped

### Requirement: A document reports what an adaptive surface costs
The memory roll-up SHALL account for an adaptive surface separately from the flat mesh layers, and SHALL separate its authoritative content from its rebuildable caches — the spatial partitions, derived normals and preview staging.

A host answering a memory warning needs to know which part it may release. Authoritative topology and attributes SHALL never be reported as rebuildable.

#### Scenario: The roll-up separates content from cache
- **WHEN** a document holding an adaptive surface with a built spatial index is measured
- **THEN** the report names the authoritative surface bytes and the rebuildable cache bytes separately
