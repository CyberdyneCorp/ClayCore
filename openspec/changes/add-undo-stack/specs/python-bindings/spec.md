# python-bindings — undo and redo

Delta for `add-undo-stack`.

## ADDED Requirements

### Requirement: Undo from Python
The module SHALL expose an opt-in undo stack per document: enabling it, `undo`, `redo`, the undo and redo depths, and grouping so a burst of edits undoes as one step. With no stack attached a document SHALL behave exactly as it does without this feature. Once attached, every editing entry point SHALL record its own inverse, so no reachable edit escapes undo.

#### Scenario: Undo restores the previous state exactly
- **WHEN** an edit is performed on a document with undo enabled and then undone
- **THEN** the document serializes bit-identically to its state before the edit

#### Scenario: Redo reapplies
- **WHEN** an edit is undone and then redone
- **THEN** the document matches the state after the original edit, and the redo depth returns to zero

#### Scenario: A stroke undoes as one step
- **WHEN** N point-append edits are made to one stroke and undo is called once
- **THEN** all N points are gone and the undo depth drops by one

#### Scenario: Grouped edits undo together
- **WHEN** several edits are bracketed by begin/end group and undo is called once
- **THEN** every edit in the group is reversed

#### Scenario: A new edit clears the redo stack
- **WHEN** an edit is undone and a different edit is then performed
- **THEN** the redo depth is zero

#### Scenario: Undo costs nothing when unused
- **WHEN** a document is edited without undo enabled
- **THEN** no inverse is recorded and the undo depth entry point reports the feature is off
