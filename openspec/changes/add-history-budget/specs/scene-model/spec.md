# scene-model

## ADDED Requirements

### Requirement: The undo history is measurable and bounded
The undo history SHALL report what it costs and SHALL accept a memory budget.

A host SHALL be able to read, in one call, the bytes held by the undo and redo stacks and the depth of each. Bytes SHALL account for what the entries OWN — a recorded node's deformer chain and stroke points included — and not only the size of the command variant, because the entries that matter are the ones holding heap payloads.

A host SHALL be able to set a byte budget on the history. When the budget is exceeded the OLDEST undo entries SHALL be dropped until it is met. Dropping SHALL be from the far end only: the most recent step SHALL always be undoable while the history is non-empty, so a budget can never make the next undo fail.

A host SHALL be able to trim the history on demand without setting a budget, for a platform that reports memory pressure and expects an immediate response.

Truncation SHALL be observable and SHALL NOT be an error. A host SHALL be able to tell that the history no longer reaches as far back as it did, so it can present the horizon rather than let a user search for a step that is gone.

An unset budget SHALL mean unbounded, so a host that never sets one behaves exactly as before.

#### Scenario: The history reports what it holds
- **WHEN** a document with undo enabled receives a sequence of edits
- **THEN** the reported byte count grows, and an edit whose inverse carries a whole node reports more than one whose inverse carries an id

#### Scenario: A budget evicts the oldest step
- **WHEN** a budget is set below what the history currently holds
- **THEN** entries are dropped from the oldest end until the budget is met, the reported depth falls, and undo still reverses the most recent edit exactly

#### Scenario: The newest step survives any budget
- **WHEN** a budget smaller than a single entry is set and one edit is then performed
- **THEN** that edit is still undoable

#### Scenario: Trimming is not an error
- **WHEN** a host trims the history and then queries it
- **THEN** the call succeeds, the depth reflects the trim, and the host can distinguish a trimmed history from an empty one

#### Scenario: No budget means no change
- **WHEN** no budget is set
- **THEN** the history grows without eviction and every recorded step remains undoable
