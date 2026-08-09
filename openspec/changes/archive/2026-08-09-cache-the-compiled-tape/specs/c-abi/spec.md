# c-abi — the document remembers what it compiled

Delta for `cache-the-compiled-tape`.

## ADDED Requirements

### Requirement: A document reuses its compiled tape until it changes
A document SHALL compile its tape once and reuse it for every read until something changes what that tape would contain. Reads are on the interactive path and compiling is proportional to the whole document, which grows with every brush stamp, so recompiling per read makes looking at a sculpt cost more the longer it has been worked on.

The tape picking uses excludes ghosted layers and is therefore a different tape; it SHALL be remembered separately rather than sharing one slot that would be rebuilt alternately by the two.

#### Scenario: Repeated reads of an unchanged document compile once
- **WHEN** the field is read many times without any intervening edit
- **THEN** every read returns what a fresh compile would have returned

#### Scenario: Picking and evaluation do not evict each other
- **WHEN** picking and field evaluation are interleaved on an unchanged document
- **THEN** neither causes the other to recompile

### Requirement: Every mutation is visible to the next read
Any entry point that changes what the compiled tape would contain SHALL invalidate the remembered tape. This includes edits applied through the command vocabulary, undo and redo, and any layer added outside that vocabulary.

Failing to invalidate is silent: the call succeeds, nothing reports an error, and every later read answers with the field as it was before the edit. An entry point that invalidates unnecessarily merely recompiles, which is the behaviour that existed before any of this was remembered — so where it is not obvious, the tape SHALL be invalidated.

#### Scenario: Each mutating entry point is reflected
- **WHEN** the field is read, mutated through any entry point that changes it, and read again
- **THEN** the second read differs from the first

#### Scenario: Undo restores the previous field exactly
- **WHEN** an edit is undone
- **THEN** the field reads exactly as it did before that edit, and redoing restores it again

#### Scenario: Ghosting changes picking and not evaluation
- **WHEN** a layer is ghosted
- **THEN** picking stops reporting it while the evaluated field is unchanged

### Requirement: A document stays readable from several threads at once
Reading a document from more than one thread concurrently SHALL remain safe. It was safe before the tape was remembered, because compiling took the document by const reference and returned a fresh result, and remembering the result SHALL NOT take that away.

A reader SHALL receive a snapshot that stays valid for the duration of its call, so that another thread invalidating and rebuilding cannot pull the tape out from under it.

#### Scenario: Concurrent readers agree
- **WHEN** several threads evaluate and pick against one unchanged document at once
- **THEN** every reader gets the same answer a single-threaded reader would
