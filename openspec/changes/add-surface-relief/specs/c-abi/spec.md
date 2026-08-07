# c-abi — surface relief

Delta for `add-surface-relief`.

## ADDED Requirements

### Requirement: Relief is reachable as an ordinary op
Relief SHALL be a combine op on the existing vocabulary, so that an app places a relief item exactly as it places any other item and gets undo, coalescing, serialization, picking and masking without any of them learning about it.

It SHALL follow the existing parameter convention rather than introducing one: the blend_k field carries the mode's depth, and the item's rounding carries the falloff width, as the groove and tongue modes already do.

#### Scenario: A relief item is placed like any other
- **WHEN** a C caller adds an item with the relief op and an amplitude
- **THEN** the document's field shows the surface displaced over that item's region

#### Scenario: It survives a save
- **WHEN** a document containing a relief item is saved and reloaded
- **THEN** the field is unchanged
