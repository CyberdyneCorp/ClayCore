# file-io — the container carries a level stack

Delta for `add-multi-resolution`.

## ADDED Requirements

### Requirement: A voxel layer's finer levels are stored as offsets
A voxel grid's stream SHALL open with the COARSEST level in the layout it already had, and any further level SHALL follow as a tagged tail carrying only that level's per-cell offsets — the cells whose value differs from the cell above them.

Only the offsets are stored because everything else is reproducible by subdividing, so a level that carries no detail costs a count and nothing else. Storing every level in full would multiply a document's size by eight per level for content that is derivable, which is the size question the proposal left open.

A grid with a single level SHALL write no tail at all, so its bytes are exactly the bytes it wrote before levels existed.

#### Scenario: A one-level grid is byte-identical
- **WHEN** a grid that was never given a second level is serialised
- **THEN** the bytes are identical to those the same grid produced before levels existed

#### Scenario: A stack round trips
- **WHEN** a grid with several levels is saved and reloaded
- **THEN** every level holds the same cells, the active level is the one that was saved, and saving again produces identical bytes

#### Scenario: A malformed tail is refused
- **WHEN** a stream's level tail is truncated, names an impossible level count, or carries a palette index the file does not hold
- **THEN** the grid is refused as malformed rather than loaded partially built

### Requirement: A tail may not cost more than the file pays for
The reader SHALL charge a tail's declared depth against the content the file actually supplied, and SHALL refuse one whose levels it would have to materialise beyond a fixed ceiling, before building any of them.

Storing only the offsets means a tail stays small however deep the stack it names, and every level above the coarsest is rebuilt by subdividing — so a fixed-size tail asks for eight times the cells per level it declares. A depth limit alone does not bound that: a few hundred bytes claiming the maximum depth over a modest coarsest level is a request for more cells than a machine holds. The check is exact rather than an estimate, because subdivision is exact.

#### Scenario: A tiny file cannot ask for an unbounded grid
- **WHEN** a stream's tail declares a depth whose subdivision of the coarsest level would exceed the reader's ceiling
- **THEN** the grid is refused immediately, without allocating any of the levels it named

#### Scenario: A stack the file pays for still opens
- **WHEN** a stream's declared depth is within what the coarsest level's content justifies
- **THEN** it loads normally, so the guard refuses the malformed case and not the format

### Requirement: A reader that predates levels opens the document at the coarsest level
The container's major version SHALL NOT change, and no new chunk type is introduced: the tail lives inside the existing voxel chunk, after the point at which a reader written before levels stops. That reader SHALL open the document, read the coarsest level, and ignore the tail — it SHALL NOT fail, and it SHALL NOT misread the tail as chunk data.

The container minor and the scene minor SHALL both advance, bound by the static assertion that already keeps them together, because the container's content changed even though the scene payload did not. A reader that predates the tail SHALL lose the finer levels if it saves the document again, and the format notes SHALL say so.

#### Scenario: An older reader opens a newer document
- **WHEN** a document whose voxel layers carry several levels is opened by a reader written against the previous minor
- **THEN** it opens with each voxel layer at its coarsest level, and nothing else in the document is affected

#### Scenario: A newer reader opens an older document
- **WHEN** a document written before levels existed is loaded
- **THEN** every voxel layer has exactly one level and is otherwise exactly what it was
