# scene-model — layer edits and layer properties in the history

Delta for `add-mesh-sculpt-layers`.

## ADDED Requirements

### Requirement: Layer content and layer properties are both reversible
The one undo history SHALL reverse both a stroke written into a sculpt layer and a change to a layer's properties — rename, strength, visibility, order, lock, add, remove, merge and bake.

Property changes are small and fully describable, so they SHALL be recorded as reversible steps rather than as barriers. An operation that genuinely destroys information SHALL still be a barrier, and SHALL name what it destroyed for a host to show.

Content and property payloads SHALL be separate step kinds or separately tagged, so that undo memory is measurable and journal replay can validate each.

The history SHALL reach the layer stack through a resolver supplied by the owner above it, as the existing kinds do.

#### Scenario: A slider undoes
- **WHEN** a layer's strength is changed and undone
- **THEN** the previous strength is restored and the evaluated surface matches what it showed before

#### Scenario: A stroke into a layer undoes without touching the stack
- **WHEN** a stroke written into a layer is undone
- **THEN** the layer's detail is restored and its name, order, strength and visibility are unchanged

### Requirement: A document reports what layers cost, separately from caches
The memory roll-up SHALL report sculpt-layer content separately from the evaluated caches derived from it, and SHALL report it as authoritative.

An evaluated stack cache is rebuildable and may be released under pressure. Layer content is the artist's work and SHALL NEVER be reported as rebuildable, nor released to satisfy a budget.

#### Scenario: A trim never costs a layer
- **WHEN** every rebuildable cache is released under pressure
- **THEN** the sculpt-layer content bytes are unchanged and the evaluated surface reconstructs identically
