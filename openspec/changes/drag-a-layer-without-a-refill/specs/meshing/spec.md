# meshing — one SDF layer's surface

Delta for `drag-a-layer-without-a-refill`.

## ADDED Requirements

### Requirement: One SDF layer can be meshed on its own
The mesher SHALL be able to mesh a single named SDF layer, producing that layer's surface in world space under that layer's own transform, with the same mesher selection, voxel sizing and attribute behaviour as meshing the whole document.

This is meshing THE FIELD of one layer and is a different call from borrowing an imported mesh layer's triangles, which returns geometry the library did not produce. Naming a layer that is not an SDF layer SHALL be refused rather than falling back to the borrow.

A hidden layer SHALL be meshable by name: the caller named it, which is a stronger statement than the visibility flag, and refusing would make "mesh this layer" depend on a flag the caller can see for itself.

#### Scenario: A single-layer document meshes the same either way
- **WHEN** a document holding one visible SDF layer is meshed whole and then by name, at the same parameters
- **THEN** the two meshes are identical

#### Scenario: A layer meshes under its own transform
- **WHEN** an SDF layer is placed away from the origin and meshed by name
- **THEN** its vertices are in world space, and re-placing the layer rigidly and meshing again gives the previous vertices mapped through the placement matrix

#### Scenario: A non-SDF layer is refused
- **WHEN** a voxel or mesh layer is named
- **THEN** the call is refused and no mesh is produced
