# python-bindings — the stroke engine

Delta for `add-brush-stroke-engine`.

## ADDED Requirements

### Requirement: Strokes from Python
The module SHALL expose a preset, resolution of an (N, samples) array of stroke samples into stamps, and application of a stroke to a voxel grid or an SDF layer with an optional mask.

#### Scenario: Resolving a stroke returns stamps
- **WHEN** a script resolves a stroke from an array of samples
- **THEN** it receives one entry per stamp, with position, radius and strength

#### Scenario: A stroke applied to a layer is undoable
- **WHEN** a script applies a stroke to an SDF layer with undo enabled and undoes it
- **THEN** the document is restored exactly
