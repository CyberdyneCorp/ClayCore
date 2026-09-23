# file-io

## ADDED Requirements

### Requirement: A voxel chunk and its layer stay matched

A document SHALL write a voxel chunk only for a layer id that is present as a
voxel-kind layer, and SHALL drop on load any voxel chunk naming none. This is
the rule mesh chunks already follow, applied to the representation that now
needs it.

Cell data SHALL NOT be discarded when a voxel layer is removed. The inverse of a
removal restores the layer by value and cannot carry a grid, so the retained
cells are what makes a redo restore content rather than an empty layer.

The pairing matters because layer ids are NOT monotonic across a reload: a
document derives its next id from the layers it contains, so an orphaned chunk
that reached a file could be captured by a later, different layer and hand it a
sculpt it never made.

#### Scenario: An orphaned grid is not written
- **GIVEN** a document holding cells for a layer it no longer contains
- **WHEN** it is saved
- **THEN** the stream carries no voxel chunk for that layer

#### Scenario: An unmatched chunk is dropped
- **GIVEN** a stream carrying a voxel chunk whose layer id names no voxel layer
- **WHEN** it is loaded
- **THEN** the chunk is dropped and the load succeeds

#### Scenario: A new layer in a reloaded document is empty
- **GIVEN** a document saved after a crossing was undone
- **WHEN** it is reloaded and a voxel layer is created
- **THEN** the new layer holds no cells, whatever id it is given

#### Scenario: An undone creation is still redoable within a session
- **GIVEN** a voxel layer whose creation has been undone
- **WHEN** the document is redone before being saved
- **THEN** the layer returns with the cells it held
