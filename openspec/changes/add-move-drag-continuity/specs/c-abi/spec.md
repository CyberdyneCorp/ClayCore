# c-abi — move drag continuity

Delta for `add-move-drag-continuity`.

## ADDED Requirements

### Requirement: Previewing a move across the C ABI
The ABI SHALL expose previewing a drag, following the size-query convention every other list-returning entry point here uses. It SHALL validate its arguments exactly as applying the move does, so a preview cannot succeed where the move would be refused.

#### Scenario: A host previews before committing
- **WHEN** a host previews a drag
- **THEN** it receives the nodes the move would warp, and the document is unchanged

#### Scenario: A preview refuses what the move refuses
- **WHEN** a preview is asked with a radius that is not positive, an unknown layer, or a malformed descriptor
- **THEN** it fails with the same code applying the move would give
