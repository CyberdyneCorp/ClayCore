## MODIFIED Requirements

### Requirement: A topology gesture is one sparse undo step
The library SHALL record the connectivity, geometry and attribute changes a gesture made as a reversible delta, and SHALL NOT snapshot the surface.

The record SHALL be COALESCED over the gesture: one entry per element, keeping the first `before` and the last `after`, so its size follows the elements touched rather than the stamps taken.

Reverting SHALL restore the surface BIT-EXACTLY, connectivity included, and reverting then re-applying SHALL each be idempotent.

Bit-exact SHALL include the DERIVED state an operator rewrites, face and vertex normals among them. Every pass that writes an element SHALL note it before the write and sync it after the last write of that pass, including normals recomputed after a position change. The record's `after` end SHALL therefore equal the live surface immediately after capture. Exactness is over LIVE elements. Slots a gesture created and a revert retired stay allocated, because a pool never compacts.

The delta SHALL encode and decode through a versioned format whose decoder REJECTS hostile or truncated counts before allocating.

One gesture SHALL be one undo step even when it contains hundreds of stamps and thousands of topology operations, and a step spanning a scene command and a topology delta SHALL undo as one.

#### Scenario: A stroke reverts exactly
- **WHEN** a stroke that split, collapsed and flipped many edges is reverted
- **THEN** the surface is bit-identical to before the stroke, connectivity included

#### Scenario: Undo size follows what was touched
- **WHEN** a long gesture stamps repeatedly over one small region
- **THEN** the delta records each affected element once, and its size does not grow with the number of stamps

#### Scenario: The relax pass leaves an exact record
- **WHEN** an adaptive stroke runs with `relax_after_remesh` enabled and its record is captured
- **THEN** every entry's `after` end equals the live surface, normals included, and undoing then redoing the stroke reproduces the exported normals of both ends exactly

## ADDED Requirements

### Requirement: Replaying a gesture keeps the sculptor in step
Replaying a recorded gesture through the sculptor that owns the surface's chunked index SHALL leave that index covering exactly the live faces. Replay SHALL mark the chunks holding the faces it restored or removed as dirty. It SHALL do this incrementally, from the elements the record names, and SHALL NOT require `rebuild_index`.

A surface SHALL carry a restorable mark that identifies its state: a lineage unique to the surface instance, and an epoch that advances whenever a revision advances. A recorded gesture SHALL hold the mark before its first stamp and the mark after its last. Reverting SHALL require the surface to be at the `after` mark, and applying SHALL require the `before` mark. A replay onto the other mark SHALL change nothing and succeed. Any other mark SHALL be refused BEFORE anything is written. Capture into a non-empty record whose `after` mark differs from the surface SHALL also be refused.

**The mark, not a comparison of content, is the guard.** Two strokes on opposite sides of a surface still share slots, because the later one reuses slots the earlier one freed. So a replay that is not last-in first-out can corrupt a surface that no spatial test would connect to the edit. A content comparison catches the overlaps it can see, but cannot prove a replay sound across an edit it did not record.

#### Scenario: The same stroke after an undo repeats the first
- **WHEN** a sculptor records a stroke, the stroke is reverted through that sculptor, and the identical stroke is stamped again
- **THEN** the surface equals the one the first stroke produced, and every live face is reachable through the index

#### Scenario: An undo is visible in the chunk stream
- **WHEN** the dirty set is cleared, a stroke is reverted, and the host reassembles the surface from the dirty chunks
- **THEN** the reassembly equals the whole-surface export

#### Scenario: A replay out of order is refused and changes nothing
- **WHEN** strokes A and B are recorded in that order and A is reverted while B is still applied
- **THEN** the revert is refused, and the surface, its revisions and its index are unchanged

#### Scenario: An unrecorded edit in between is refused
- **WHEN** a stroke is recorded, a further stamp changes the surface without a record, and the recorded stroke is reverted
- **THEN** the revert is refused and nothing is written

#### Scenario: Replay is idempotent at the target
- **WHEN** a recorded stroke is reverted twice
- **THEN** the second revert succeeds, writes nothing and advances no revision
