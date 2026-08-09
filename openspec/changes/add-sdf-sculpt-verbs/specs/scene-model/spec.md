# scene-model — sculpting verbs for an SDF layer

Delta for `add-sdf-sculpt-verbs`.

## ADDED Requirements

### Requirement: Shaping verbs are commands
A shaping verb applied to a layer SHALL go through the command vocabulary, so it is undoable, refused on a protected layer, and serialised with the document like any other edit.

A document that reloads SHALL evaluate bit-identically to the one that was saved, which means the verb's region and parameters are persisted rather than its result.

#### Scenario: A shaping verb undoes exactly
- **WHEN** a shaping verb is applied and then undone
- **THEN** the field matches the document before the verb, everywhere

#### Scenario: A shaping verb survives a round trip
- **WHEN** a document containing shaping verbs is saved and reloaded
- **THEN** it evaluates bit-identically and reserialises to identical bytes
