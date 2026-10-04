## ADDED Requirements

### Requirement: A voxel grid can be cloned into a grid the caller owns
The ABI SHALL provide `clay_voxel_grid_clone`, which accepts an owned or a
borrowed grid handle and returns a new handle the caller owns and frees with
`clay_voxel_grid_destroy`. The clone SHALL carry every resolution level, the
active level, the palette and the sculpt layers of the source.

The clone SHALL be in no document. Edits to it SHALL reach neither the source
grid nor the document's undo history, and a sculpt layer that is recording in
the source SHALL arrive closed. A borrowed source SHALL remain the document's:
destroying it SHALL still be refused after it has been cloned.

#### Scenario: A borrowed layer's grid is cloned whole
- **WHEN** a host clones a borrowed grid holding two levels with the finer one active, a multi-entry palette and an open sculpt layer
- **THEN** the clone reports the same level count, active level, per-level occupied counts, palette colours, cells and sculpt layers, and reports no sculpt layer recording

#### Scenario: Editing the clone leaves the document alone
- **WHEN** a host edits the clone of a borrowed grid in a document with undo enabled
- **THEN** the document's grid, its sculpt-layer record and its undo depth are unchanged, destroying the clone succeeds, and destroying the borrowed source is still refused

### Requirement: A voxel grid's occupied cells can be read in one call
The ABI SHALL provide `clay_voxel_get_occupied`, which writes every occupied
cell of the active level and its palette index, ordered by z, then y, then x,
using the size-query pattern: with both buffers NULL it SHALL report the count
without walking any cell, and a capacity below the count SHALL be
`CLAY_ERROR_BUFFER_TOO_SMALL` with the needed count reported and nothing
written. It SHALL walk the chunks that hold material rather than the bounding
box, and SHALL report the cells a partially refined level inherits from its
parent.

#### Scenario: The bulk read matches a box walk
- **WHEN** a host reads a sparse grid with `clay_voxel_get_occupied`, on a whole level and on a partially refined one
- **THEN** the cells, indices and order equal those a `clay_voxel_get` walk of the bounding box finds, and the count equals `clay_voxel_occupied_count`

### Requirement: The header states which voxel reads may leave the interface thread
`clay.h` SHALL state, beside `clay_voxel_grid_clone`, `clay_voxel_get_occupied`,
`clay_item_volume_from_voxels` and `clay_voxel_to_layer`, which of them may run
on a worker: on an owned grid the reads touch no document and may run on any
thread; on a borrowed grid they are reads of the document, safe against a const
document and not concurrently with a mutating `clay_document_*` /
`clay_voxel_*` call. It SHALL state that two concurrent readers of a grid whose
bounds cache is cold race, and how a host avoids it.

#### Scenario: A host finds the threading contract beside the call
- **WHEN** a host integrator reads the declarations of the clone, the bulk read and the two conversion calls
- **THEN** each states its thread footing, and the clone's names the bounds-cache race and its remedy
