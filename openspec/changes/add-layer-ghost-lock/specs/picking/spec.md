# picking — ghosted layers

Delta for `add-layer-ghost-lock`.

## ADDED Requirements

### Requirement: Ghosted layers are not picked
Ray and surface-snapping queries SHALL ignore ghosted layers, exactly as they ignore hidden ones, while still evaluating them. A locked layer SHALL remain pickable: locking protects against edits, not against selection.

#### Scenario: A ghost in front does not steal the hit
- **WHEN** a ray would hit a ghosted layer before reaching a visible one
- **THEN** the hit reports the visible layer

#### Scenario: Ghosting the only layer means no hit
- **WHEN** every layer a ray would hit is ghosted
- **THEN** the query reports no hit

#### Scenario: A locked layer is still pickable
- **WHEN** a ray hits a locked layer
- **THEN** the hit reports that layer
