# c-abi — placing a layer, and dragging one

Delta for `drag-a-layer-without-a-refill`.

## ADDED Requirements

### Requirement: The ABI reports what a layer placement would do
A caller SHALL be able to learn how a PROPOSED layer placement classifies — rigid, similarity or general — and to obtain the matrix taking the layer's current placement to that one, as a column-major affine matrix.

It SHALL be a QUERY that changes nothing, rather than an output added to the placement calls. That shape satisfies the two properties this requirement is for by construction rather than by care: the existing entry points keep their signatures untouched, and asking cannot alter the document, the invalidation or a later refill because it does not write. A caller asks with the placement it is about to set; asking afterwards is legal and answers the identity, which is true and useless.

A proposed placement SHALL be refused on the same terms the placement calls refuse it, so a report cannot be obtained for a placement that could not then be set.

Reporting SHALL NOT change what is invalidated. A host that ignores it SHALL see nothing different, which is what lets the report ship before the engine acts on it.

#### Scenario: A rigid placement reports its matrix
- **WHEN** a layer at the origin is placed with a rotation and a translation and the report is asked for
- **THEN** the classification is rigid and the matrix maps the layer's previous world-space bound onto its new one

#### Scenario: A general placement says so
- **WHEN** a layer is placed with a per-axis scale and the report is asked for
- **THEN** the classification is general

#### Scenario: Asking changes nothing
- **WHEN** one document is asked for a report many times and an otherwise identical document is not asked at all
- **THEN** the two evaluate identically and save to the same bytes

#### Scenario: A malformed placement is refused
- **WHEN** a report is asked for with a zero rotation axis, or a scale that is not positive
- **THEN** the call is refused, as setting that placement would be

### Requirement: A layer placement can be dragged as one gesture
The ABI SHALL offer a placement GESTURE for a layer: an opening call, any number of updates, and a closing call.

While a gesture is open, an update SHALL record the placement and SHALL NOT invalidate anything and SHALL NOT recompile. The closing call SHALL apply the final placement as ONE command and perform ONE invalidation, so a drag of N frames costs one refill rather than N.

The document a caller reads while a gesture is open SHALL be the document as it was when the gesture opened. The placement being dragged is the host's to draw and is not yet an edit — nothing in the document, and nothing a refill returns, SHALL reflect it before the gesture closes.

Closing SHALL be undoable as one step, whatever the number of updates, so an artist takes back a drag and not a frame of it.

A gesture abandoned — the document destroyed, or the gesture cancelled — SHALL leave the placement the gesture opened with. A gesture SHALL be refused on a protected layer on the same terms as the ordinary placement call, and refused at the opening call rather than at the close, so a host learns before the artist has dragged anything.

An edit to the document through any other entry point while a gesture is open SHALL be refused rather than interleaved, because the gesture's whole claim is that the document did not change.

#### Scenario: A drag costs one invalidation
- **WHEN** a gesture opens on a populated SDF layer, sixty updates are applied, and the gesture closes
- **THEN** exactly one command is recorded, exactly one invalidation is performed, and the bricks refilled across the whole gesture are those one placement would have dirtied

#### Scenario: The document does not move until the drag ends
- **WHEN** updates are applied and the document is sampled between them
- **THEN** the values are those of the placement the gesture opened with

#### Scenario: A drag is one undo step
- **WHEN** a gesture of many updates closes and is undone
- **THEN** the layer returns to the placement the gesture opened with in a single undo

#### Scenario: An abandoned gesture leaves nothing behind
- **WHEN** a gesture is opened, updated and cancelled
- **THEN** the layer carries the placement it had before the gesture opened, and nothing was invalidated

#### Scenario: A protected layer refuses at the open
- **WHEN** a gesture is opened on a locked or ghosted layer
- **THEN** the call is refused, and no gesture is open

#### Scenario: An edit during a gesture is refused
- **WHEN** an item is added to any layer while a placement gesture is open
- **THEN** the edit is refused and the document is unchanged

### Requirement: The document is evaluable with a layer excluded, or as that layer alone
So a host can draw a placement preview without evaluating the composite, the ABI SHALL let an evaluation name a layer and ask for either the document WITHOUT it or that layer ALONE, on both the brick path and the mesh path.

The two SHALL be exact complements under the document's hard union: at every point, the minimum of the two results SHALL equal what the whole document evaluates to at that point, for a document of visible SDF layers. This is the property that makes drawing them separately a decomposition rather than an approximation.

Naming a layer that does not exist, or one that is not an SDF layer, SHALL be refused rather than treated as naming nothing. A HIDDEN SDF layer SHALL be accepted by name, on the same reading meshing one uses: the caller named it, which says more than the visibility flag does.

#### Scenario: The two halves recompose
- **WHEN** a document of three visible SDF layers is evaluated whole, then as "without layer 2" and "layer 2 alone" over the same lattice
- **THEN** the pointwise minimum of the two parts equals the whole document's values

#### Scenario: One layer alone ignores the others
- **WHEN** a layer is evaluated alone and another layer is then edited
- **THEN** re-evaluating that layer alone returns the values it returned before

#### Scenario: A layer that is not there is refused
- **WHEN** an evaluation names a layer id the document does not hold, or a voxel or mesh layer
- **THEN** the call is refused and writes nothing
