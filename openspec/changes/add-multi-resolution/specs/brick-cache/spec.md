# brick-cache — sculpt at more than one resolution

Delta for `add-multi-resolution`.

## ADDED Requirements

### Requirement: The cache states which level it holds
The brick cache is a sparse narrow band around one lattice. With more than one level present it SHALL state whether it caches every level or only the finest, and a level change SHALL invalidate exactly the bricks it affects rather than the whole cache.

#### Scenario: A level change does not drop the whole cache
- **WHEN** the active level changes
- **THEN** bricks unaffected by the change keep their generation and are not resubmitted
