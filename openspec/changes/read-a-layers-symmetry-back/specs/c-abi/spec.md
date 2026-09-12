## ADDED Requirements

### Requirement: A layer's symmetry can be read, not only written

A layer's mirror and radial symmetry SHALL be readable through the C ABI.

Every other piece of layer state is readable beside being writable. Symmetry
was not, and a caller that cannot ask has to remember — which makes the
caller's memory a second source of truth that the document can invalidate
without telling it. An undo that reverts a symmetry edit leaves such a caller
believing a symmetry the document does not carry, and the next edit is made
through it.

A reader SHALL take what its writer takes, so that a value read can be written
back unchanged. Where the writer expresses an axis as separate flags, the
reader SHALL answer in those terms rather than in an internal encoding.

Every output SHALL be optional, so that a caller may use the call to validate a
layer without supplying a buffer.

**Reading SHALL NOT be editing.** A layer that is locked, hidden or otherwise
protected SHALL answer normally, while setting its symmetry stays refused. A
layer that carries no symmetry SHALL answer with symmetry off rather than
refusing, because that is the true answer. A layer of a kind that cannot
express symmetry SHALL be refused rather than answered with zeroes, which would
be indistinguishable from a real answer.

#### Scenario: Reading back what was set
- **WHEN** a layer's mirror or radial symmetry is set and then read
- **THEN** the values returned are the values set, and writing them back changes nothing

#### Scenario: A layer with no symmetry
- **WHEN** the symmetry of a layer that carries none is read
- **THEN** it answers with symmetry off rather than refusing

#### Scenario: Reading follows an undo
- **GIVEN** a layer whose symmetry was set and then undone
- **WHEN** its symmetry is read
- **THEN** it reports the symmetry the document now carries, not the one that was set

#### Scenario: A protected layer answers but does not change
- **WHEN** the symmetry of a protected layer is read, and then set
- **THEN** the read succeeds and the set is refused

#### Scenario: A layer that cannot carry symmetry
- **WHEN** the symmetry of a layer whose kind cannot express it is read
- **THEN** the call is refused rather than answered with zeroes
