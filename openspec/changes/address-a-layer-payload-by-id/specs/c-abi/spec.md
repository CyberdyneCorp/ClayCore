# c-abi — a layer's payload is reachable by the layer's id

Delta for `address-a-layer-payload-by-id`.

## ADDED Requirements

### Requirement: A layer's payload is reachable by layer id
The C API SHALL provide an id-addressed accessor for each representation that carries a payload beside the document: one that borrows a voxel layer's grid and one that borrows a mesh layer's geometry, each taking the layer id and no name.

This is a requirement rather than a convenience because the ABI already tells a host to hold the id — the rename call states that ids are stable across a save and load while names are not a key anything enforces — and for these two representations that advice could not be followed: the only route back to the payload of a reopened document's layer keyed on the NAME. The consequence is silent rather than loud. Two layers sharing a name shadow one another, the by-name lookup SUCCEEDS on the first in stack order, and an edit lands on the wrong layer with no error for the host to check. A host's only defence was to invent a uniqueness rule for one representation that the create calls have never asked for.

The by-name lookups SHALL remain and SHALL keep their behaviour, since they answer the question they are asked and a document with one layer of a given name is the ordinary case.

The addition SHALL be purely additive: no existing signature changes, no existing call's meaning moves, and no document format version moves, since layer ids are already stable across a save and reload.

#### Scenario: Two layers sharing a name are told apart by id
- **GIVEN** a document with two voxel layers carrying the same name and different cells, and two mesh layers carrying the same name and different geometry
- **WHEN** each layer's payload is fetched by its own id
- **THEN** each fetch reaches that layer's own payload
- **AND** the by-name lookup reaches only the first of each pair in stack order

#### Scenario: An id still reaches the payload after a save and reload
- **WHEN** a document holding two same-named voxel layers is saved and loaded into a fresh document, and each id is fetched again
- **THEN** each id reaches the same payload it reached before the round trip

#### Scenario: A rename does not move what an id reaches
- **WHEN** a layer is renamed, including onto a name another layer already carries
- **THEN** its id reaches the same payload it reached before the rename

### Requirement: An id-addressed payload lookup refuses what it cannot answer
The id-addressed accessors SHALL return the not-found refusal when no layer carries the id, when the layer carrying it is of another representation, and when the layer is of the right representation but has no payload entry.

The layer SHALL be resolved in the DOCUMENT first, not in the payload table. A payload deliberately outlives its layer: undoing the creation of a voxel layer removes the layer and KEEPS the grid beside the document, so that a redo brings the layer back with its cells. An accessor that resolved the id in the payload table alone would therefore hand back a grid whose layer is currently undone — a state the by-name lookup reports as not found, and reaching it would be a new hole rather than a new capability. Whatever the by-name lookup refuses for a given layer, the by-id lookup SHALL refuse.

The output pointer SHALL be REQUIRED, and a null one SHALL be refused as an invalid argument. This deliberately differs from the by-name lookups, where a null output is meaningful because the call doubles as an existence probe and still reports the resolved id. Here the caller supplied the id and the borrowed handle is the only answer the call has, so a null output asks nothing; the layer-info query is the call that answers whether a layer exists and what representation it is. A null document SHALL be refused the same way, and a refused call SHALL write nothing through the output pointer.

#### Scenario: An id of the wrong representation is not found
- **WHEN** a mesh layer's id is passed to the voxel accessor, or a voxel layer's id to the mesh accessor, or an SDF layer's id to either
- **THEN** the call returns not-found and writes nothing

#### Scenario: An unknown id is not found
- **WHEN** an id no layer carries is passed to either accessor
- **THEN** the call returns not-found and writes nothing

#### Scenario: An undone creation is not reachable by id
- **GIVEN** a voxel layer whose creation has been undone, so the layer is gone and its grid is still held beside the document
- **WHEN** its id is passed to the voxel accessor
- **THEN** the call returns not-found, as the by-name lookup does
- **AND WHEN** the creation is redone
- **THEN** the same id reaches the same grid, with the cells it held

#### Scenario: A layer whose payload is absent is not found
- **GIVEN** a loaded document carrying a voxel layer whose payload did not come with it
- **WHEN** that layer's id is passed to the voxel accessor
- **THEN** the call returns not-found rather than borrowing a payload that is not there

#### Scenario: The refusals are typed
- **WHEN** either accessor is called with a null document, or with a null output pointer
- **THEN** the call returns the invalid-argument refusal, and nothing is written
