# c-abi — expose layer enumeration

Delta for `expose-layer-enumeration` (#69).

## ADDED Requirements

### Requirement: A host can discover a document's layers
The C API SHALL let a host enumerate a document's layers by a count and an index, where the index is STACK POSITION — the order evaluation uses and `clay_document_move_layer` sets — so the set and the order of a reloaded document are recoverable together. An index at or beyond the count SHALL be a typed not-found, which is how a host walks without a sentinel. Ids SHALL remain stable across a save and reload, and enumeration SHALL go through the index rather than the id space, because a removal leaves a gap in the ids.

Everything settable about a layer SHALL be readable back: a versioned info descriptor (leading `struct_size`, per the descriptor convention) SHALL carry the layer's id, representation, stack index, visibility and both protection flags in one call, and the layer's name SHALL be returned by the size-query pattern, since it is the one layer property without a fixed size. The representation SHALL be declared as an enumeration whose values match the layer record's kind byte in a saved document, appended and never renumbered.

Reading is not editing: a ghosted, locked or hidden layer SHALL answer every discovery query normally. The addition SHALL be purely additive — no existing signature changes, no struct grows, no enumerator's value changes.

#### Scenario: A reloaded document comes back whole
- **WHEN** a document with SDF, voxel and mesh layers — renamed at creation, one hidden, one locked, one ghosted, reordered with a move, one layer removed — is saved, reloaded and enumerated
- **THEN** the count, the stack order and every layer's id, name, representation, visibility and protection match what was saved, in a number of calls proportional to the layer count

#### Scenario: Stack order is the enumeration order
- **WHEN** a layer is moved to a new stack position and the document is enumerated
- **THEN** the enumeration reflects the move, and each layer's reported stack index is the index that would re-address it

#### Scenario: A removed layer's id is a gap, not a guess
- **WHEN** a layer is removed and the document is saved and reloaded
- **THEN** enumeration yields the surviving ids only, and the removed id is refused as not found by the info and name queries

#### Scenario: The info descriptor is versioned like every other
- **WHEN** a caller passes an info struct whose `struct_size` is zero or below the original layout
- **THEN** the call is refused as an invalid argument rather than misread

#### Scenario: The name query sizes before it writes
- **WHEN** a host queries a layer's name with a null buffer and then with a buffer of the reported size
- **THEN** it receives the required size including the terminator and then the name; a buffer that is too small is refused with the needed size reported and nothing written
