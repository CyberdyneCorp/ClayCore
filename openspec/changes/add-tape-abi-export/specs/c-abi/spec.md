# c-abi — the compiled tape crosses the boundary

Delta for `add-tape-abi-export`.

## ADDED Requirements

### Requirement: The compiled tape is exportable
A consumer SHALL be able to obtain a document's compiled tape through the C ABI: the instruction array, the parameter array and the out-of-line blob, in the layout the published kernel headers define, since the consumer's evaluator is compiled from those same headers.

The export SHALL include what an evaluator cannot derive from the buffers alone: the field info the safe step scale comes from, and the tape's bounds. A host that guesses its step scale draws a wrong frame; one that guesses its bounds draws a slow one.

The export SHALL carry the document revision the tape was compiled at, so a consumer can tell whether the copy it holds is still current without comparing buffers.

Ownership across the boundary SHALL be explicit and SHALL NOT depend on the consumer noticing a mutation: no exported pointer may be silently invalidated by a subsequent edit.

The tape encoding SHALL be versioned with the published kernel package, and a version the consumer does not support SHALL be detectable and refused rather than reinterpreted.

#### Scenario: A host evaluates the exported tape
- **WHEN** a consumer exports a tape and evaluates it with `ctape_eval` from the published kernel headers at the same points the library evaluates
- **THEN** the results agree within the host-parity tolerance the fixture already gates

#### Scenario: An edit is detectable without re-reading the tape
- **WHEN** the document is edited after a tape was exported
- **THEN** the consumer can tell that its copy is stale from the revision alone

#### Scenario: An edit does not invalidate an export in use
- **WHEN** a consumer holds an exported tape and the document is edited
- **THEN** the data the consumer holds remains valid and readable for as long as the ownership rule says it does, and the rule does not depend on the consumer having observed the edit

#### Scenario: A version mismatch is refused
- **WHEN** a consumer built against a different kernel package version reads the export
- **THEN** the mismatch is detectable and the export is refused rather than interpreted under the wrong layout

#### Scenario: Exporting does not disturb evaluation
- **WHEN** a tape is exported while the same document is being evaluated from another thread
- **THEN** both proceed correctly, consistent with a document staying readable from several threads at once
