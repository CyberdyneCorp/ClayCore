# c-abi — the stroke engine

Delta for `add-brush-stroke-engine`.

## ADDED Requirements

### Requirement: Strokes across the ABI
The C API SHALL expose a versioned preset descriptor, stroke resolution through the size-query pattern, and stroke application to a voxel grid or an SDF layer with an optional mask.

#### Scenario: A stroke resolves identically through both bindings
- **WHEN** a C consumer resolves a stroke with a given preset and seed
- **THEN** the stamps match what `pyclay` produces for the same input
