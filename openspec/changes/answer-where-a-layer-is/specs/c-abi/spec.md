# c-abi — a layer's bounds answer from what the layer holds

Delta for `answer-where-a-layer-is`.

## ADDED Requirements

### Requirement: A layer's bounds answer from whichever representation it holds
`clay_layer_bounds` SHALL report the tight world-space extent of a layer's content whatever representation that content is, and SHALL NOT report the absence of bounds for a layer that holds material. A mesh layer answers from its vertex positions and a voxel layer from its occupied cells, as an SDF layer answers from its shapes.

The reason this is a requirement rather than a convenience is that the alternative is not a conservative answer, it is a WRONG one: a mesh's vertices are a box and a grid's occupied cells are a box, so "this layer is nowhere" is false for either whenever it holds anything, and a host cannot tell that answer apart from an empty layer's.

A voxel layer's extent SHALL treat a cell as the BOX it is rather than as a point, so the far corner covers the whole of the last occupied cell. A single occupied cell therefore has the extent of one cell rather than none, which is what stops a one-cell grid reporting an empty box and reading as nowhere again.

Every representation SHALL answer in WORLD space, composing the layer transform, since a caller comparing two layers, framing a camera or placing a manipulator is asking one question and would otherwise be answered in two different spaces.

A layer holding NO material SHALL still report no bounds. An empty grid is genuinely nowhere, and that is a different answer from a representation that cannot say.

The composition SHALL live where the document that owns every representation is in scope, and SHALL NOT be obtained by giving `clay::scene` sight of the voxel or mesh modules. The layering rule that withholds them is what makes "this content does not change what the document evaluates to" structural, and a bounds query is not a reason to weaken it.

#### Scenario: A mesh layer reports the mesh's own box
- **GIVEN** a mesh attached as a layer
- **WHEN** the layer's bounds and the mesh's own bounds are both read
- **THEN** the two are the same box, and the layer reports that it has bounds

#### Scenario: A voxel layer follows its occupied cells
- **GIVEN** a voxel layer with a single occupied cell at the origin
- **WHEN** its layer bounds are read
- **THEN** they span that one cell rather than collapsing to a point
- **AND** occupying a second, distant cell grows the box to cover both whole cells

#### Scenario: An empty layer is still nowhere
- **GIVEN** a voxel layer with no occupied cells
- **WHEN** its layer bounds are read
- **THEN** it reports no bounds, as an SDF layer holding no shapes does

#### Scenario: Moving a layer moves its bounds
- **GIVEN** a mesh or voxel layer with a non-identity layer transform
- **WHEN** its layer bounds are read
- **THEN** they are the content's extent under that transform, as an SDF layer's are

#### Scenario: A mesh layer's bounds are a region the mesh rasterizer accepts
- **GIVEN** a mesh layer in a document
- **WHEN** its layer bounds are passed as the region to the mesh-to-voxel rasterization
- **THEN** the call is accepted and rasterizes the geometry, rather than refusing for want of a region
