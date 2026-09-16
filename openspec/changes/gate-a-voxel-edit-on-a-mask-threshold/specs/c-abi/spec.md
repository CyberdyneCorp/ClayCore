## MODIFIED Requirements

### Requirement: Masks across the ABI
The C API SHALL expose mask creation on a layer, painting, the region operations, batch sampling through the size-query pattern, and masked voxel edits.

`clay_brush_params` SHALL carry the mask threshold as an APPENDED field under the `struct_size` pattern, so a caller compiled against the older struct passes the older size and keeps the scaling behaviour it has today. A threshold outside [0, 1] SHALL be refused with a typed error naming the field.

#### Scenario: Freezing from C
- **WHEN** a C consumer masks a region and stamps a brush across it
- **THEN** the masked cells are unchanged, matching what `pyclay` produces for the same sequence

#### Scenario: A threshold freezes a skirt from C
- **WHEN** a C consumer stamps a solid brush across a mask with a falloff, with a threshold set
- **THEN** no cell at or above the threshold is written

#### Scenario: An older caller is unaffected
- **WHEN** a C consumer passes a `struct_size` from before the field existed
- **THEN** the edit writes exactly the cells it wrote before the field existed

#### Scenario: A threshold out of range is refused
- **WHEN** a C consumer passes a threshold below zero or above one
- **THEN** the call is refused and the grid is unchanged
