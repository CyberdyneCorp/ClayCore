# c-abi — the mask field

Delta for `add-mask-field`.

## ADDED Requirements

### Requirement: Masks across the ABI
The C API SHALL expose mask creation on a layer, painting, the region operations, batch sampling through the size-query pattern, and masked voxel edits.

#### Scenario: Freezing from C
- **WHEN** a C consumer masks a region and stamps a brush across it
- **THEN** the masked cells are unchanged, matching what `pyclay` produces for the same sequence
