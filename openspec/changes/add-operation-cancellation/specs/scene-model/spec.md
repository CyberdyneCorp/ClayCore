# scene-model

## ADDED Requirements

### Requirement: An operation is either committed or abandoned
An operation that mutates a document SHALL be atomic with respect to cancellation: it SHALL either commit its whole result through the command vocabulary, or commit nothing.

A cancelled operation SHALL leave the document byte-identical to its state before the call — the same nodes, the same layer content, the same evaluated field at every point — and SHALL leave the undo and redo stacks unchanged in both depth and content. Cancellation SHALL NOT be recorded as an edit, because an edit that undoes to the same state is a step a user has to discover is empty.

This SHALL hold for every phase of a multi-phase operation. An operation that samples, redistances, compacts and then fills colour SHALL be cancellable in any of those phases with the same outcome, so the guarantee does not depend on where the user pressed Stop.

#### Scenario: Cancelling mid-bake commits nothing
- **WHEN** a consolidate is cancelled during the sampling phase, and again during the colour phase
- **THEN** in both cases the layer keeps its edit list, the document evaluates identically to a probe of the same points before the call, and no undo entry is created

#### Scenario: A cancelled operation is not an undo step
- **WHEN** a host cancels an operation and then undoes
- **THEN** the undo reverses the edit that preceded the cancelled operation, not the cancellation
