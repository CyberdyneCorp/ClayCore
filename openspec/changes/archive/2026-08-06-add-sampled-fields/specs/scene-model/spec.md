# scene-model — an item carrying a volume

Delta for `add-sampled-fields`.

## ADDED Requirements

### Requirement: An item may carry a sampled volume
An item SHALL be able to carry a sampled volume as its primitive, shared between items by reference so that instancing one costs no extra storage. A volume SHALL survive a save and reload.

#### Scenario: A volume round trips
- **WHEN** a document containing a sampled volume is saved and reloaded
- **THEN** the field is unchanged

#### Scenario: An empty volume is refused
- **WHEN** an item carries a volume with no bricks and no samples
- **THEN** it contributes nothing rather than reading unwritten data

#### Scenario: A malformed volume fails the read
- **WHEN** a saved document's volume payload is truncated
- **THEN** the read fails rather than loading an item that would silently contribute nothing

#### Scenario: An older document still reads
- **WHEN** a document written before volumes existed is loaded
- **THEN** it loads without one, and the fields written after it in the record are unchanged
