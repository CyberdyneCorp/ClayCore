# scene-model — multires in the history and the memory report

Delta for `add-mesh-multires`.

## ADDED Requirements

### Requirement: The undo history reverses a multires gesture
The one undo history SHALL reverse a gesture made at any level of a subdivision hierarchy, through a step carrying sparse per-level detail changes and reaching the surface by a resolver supplied from above.

Level lifecycle operations — adding a level, removing the highest level, switching the sculpt level — SHALL be reversible where they change persistent state, and SHALL be recorded as a barrier only where they genuinely destroy information, with what happened named for a host to show.

A journal SHALL encode, decode and replay the new kind, and journals written before this change SHALL still replay.

#### Scenario: A gesture at a level undoes as one step
- **WHEN** a stroke at the active level is undone
- **THEN** the reconstructed surface is bit-identical to before the stroke and one step was consumed

#### Scenario: Removing a level is not silently irreversible
- **WHEN** the highest level is removed
- **THEN** the operation is either reversible or recorded as a barrier naming what was lost

### Requirement: A document reports what a hierarchy costs
The memory roll-up SHALL account for a multiresolution surface with its authoritative content — the base, the hierarchy metadata and the per-level detail — separate from its rebuildable caches: reconstructed positions for inactive levels, per-level adjacency, per-level spatial indices and preview staging.

A host under memory pressure SHALL be able to identify what it may release. Authoritative detail SHALL never be reported as rebuildable.

#### Scenario: The roll-up separates detail from cache
- **WHEN** a document holding a hierarchy with several built level caches is measured
- **THEN** the report names the authoritative detail bytes and the rebuildable cache bytes separately
