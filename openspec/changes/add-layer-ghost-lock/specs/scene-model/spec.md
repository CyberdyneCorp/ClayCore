# scene-model — ghosted and locked layers

Delta for `add-layer-ghost-lock`.

## ADDED Requirements

### Requirement: A layer may be ghosted or locked
A layer SHALL carry a ghost flag and a lock flag, both off by default. A ghosted layer is still evaluated but is excluded from picking and from edits. A locked layer is still evaluated and still picked, but is excluded from edits. Neither flag SHALL change what a document evaluates to.

Both flags SHALL be settable through the command vocabulary, so that setting one is undoable and serializes with the document. A document written before the flags existed SHALL load with both off.

#### Scenario: Neither flag changes the field
- **WHEN** a layer is ghosted, or locked, and the document is evaluated
- **THEN** the field is bit-identical to the same document without the flag

#### Scenario: Setting a flag is undoable
- **WHEN** a layer is ghosted and the edit is undone
- **THEN** the layer is no longer ghosted, and the document matches what it was

#### Scenario: The flags round trip
- **WHEN** a document with a ghosted layer and a locked layer is saved and reloaded
- **THEN** both flags come back set

#### Scenario: An older document loads unprotected
- **WHEN** a document written before the flags existed is loaded
- **THEN** every layer is neither ghosted nor locked

### Requirement: Edits refuse protected layers
An edit naming a ghosted or locked layer SHALL be refused with a typed error and SHALL leave the document unchanged. It SHALL NOT be silently applied, and SHALL NOT be silently dropped: a host that greys the layer out wants the refusal, and one that does not must not quietly discard the artist's work.

Changing the flags themselves SHALL remain possible on a protected layer — otherwise locking would be irreversible.

#### Scenario: A locked layer refuses an edit
- **WHEN** an item is added to a locked layer
- **THEN** the edit is refused and the layer's edit list is unchanged

#### Scenario: A ghosted layer refuses an edit
- **WHEN** an existing node in a ghosted layer is retransformed
- **THEN** the edit is refused and the node is unchanged

#### Scenario: A protected layer can be unprotected
- **WHEN** a locked layer is unlocked and then edited
- **THEN** the unlock succeeds and the edit lands
