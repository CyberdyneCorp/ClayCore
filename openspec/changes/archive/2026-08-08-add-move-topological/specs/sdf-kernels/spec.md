# sdf-kernels — Move Topological

Delta for `add-move-topological`.

## ADDED Requirements

### Requirement: A move can be weighted by distance through the material
The library SHALL provide a move whose falloff is weighted by geodesic distance from the anchor THROUGH THE MATERIAL, rather than by Euclidean distance through space, so that parts of a form which are close in space but far along the surface are not dragged together.

The distance SHALL be solved over cells the source reports as material. Free space SHALL NOT be part of the graph, which is what stops the weight crossing a gap.

It SHALL bake, for the reason relax and flatten do: the weight is a solved field rather than a closed form, and putting one in the tape would require a deformer that reads out-of-line data, which no deformer does.

#### Scenario: A neighbouring part is not dragged
- **WHEN** a topological move is applied to one of two parts that are close in space and joined only through a distant path, with a radius that spans the gap
- **THEN** the neighbouring part is unchanged, where a Euclidean move of the same radius moves it

#### Scenario: The grabbed part still moves
- **WHEN** the same move is applied
- **THEN** the part under the anchor moves in the direction of the drag

#### Scenario: Distance runs along the material
- **WHEN** the radius is raised until it exceeds the path length through the joining body
- **THEN** the neighbouring part begins to move, because it is now within reach along the material

#### Scenario: A move that reaches nothing changes nothing
- **WHEN** the anchor is placed away from any material, or the displacement is zero
- **THEN** the result matches the source

#### Scenario: The declared steepness is measured
- **WHEN** a topological move is applied
- **THEN** the result declares the Lipschitz its samples actually have, rather than an assumed bound
