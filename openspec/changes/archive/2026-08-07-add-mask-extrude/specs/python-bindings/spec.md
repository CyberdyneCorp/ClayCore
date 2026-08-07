# python-bindings — mask extrude

Delta for `add-mask-extrude`.

## ADDED Requirements

### Requirement: Mask extrude from Python
The module SHALL let a script convert a mask to a field and extrude a masked patch of a document or of a voxel grid into a new one.

The binding SHALL state that the mask is the region — there is no region radius to supply, unlike relax and flatten — because that is the first thing a script author will go looking for.

A refusal SHALL raise rather than return an empty result.

#### Scenario: A script extracts a plate from a document
- **WHEN** a script masks part of a document's surface and extrudes it
- **THEN** it gets back a volume holding the plate, which meshes and evaluates like any other

#### Scenario: A script extracts from voxels
- **WHEN** a script extrudes a masked region of a voxel grid
- **THEN** it gets back a new grid holding the extract, with the source's colours

#### Scenario: An impossible extrude raises
- **WHEN** a script extrudes with an empty mask
- **THEN** the call raises rather than returning something empty
