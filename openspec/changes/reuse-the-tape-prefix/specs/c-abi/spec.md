## ADDED Requirements

### Requirement: An append reuses what an edit did not change
A document that is edited by appending an item SHALL rebuild its remembered tape by reusing the part the append did not touch, rather than recompiling the whole document. Appending is how a stroke works — one node per brush stamp — and recompiling the whole document per stamp makes each dab cost more the longer the sculpt has been worked on, on the path where the host is already waiting to place the next one.

Mutating a document while another thread reads it was never supported and still is not; this concerns readers racing each other, and the rebuild one of them triggers.

This SHALL NOT weaken any promise the remembered tape already makes. Every mutation SHALL still be visible to the next read; where the ABI cannot establish that an edit was an append — including undo, redo, event replay, and any layer or document change applied outside the command vocabulary — it SHALL invalidate and recompile in full, as it does today. A reader SHALL still receive a snapshot that stays valid for its whole call, so a concurrent append cannot pull the tape out from under it.

#### Scenario: A stroke's cost per dab stops growing with the document
- **WHEN** items are appended one at a time to a large document and the field is read after each
- **THEN** every read returns what a fresh compile would have returned, and the work of rebuilding the tape is proportional to the appended item rather than to the whole document

#### Scenario: An append is still visible to the next read
- **WHEN** the field is read, an item is appended, and the field is read again
- **THEN** the second read reflects the appended item

#### Scenario: Undo after an append is exact
- **WHEN** an appended item is undone and the field is read
- **THEN** it reads exactly as it did before the append, and redoing restores it again

#### Scenario: Concurrent readers are unaffected by prefix reuse
- **WHEN** several threads evaluate and pick against one document whose remembered tape is stale from appends not yet consumed
- **THEN** every reader gets the answer a single-threaded reader would, the rebuild happens once however many readers race for it, and no reader observes a partially rebuilt tape
