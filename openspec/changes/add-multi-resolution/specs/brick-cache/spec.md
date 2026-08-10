# brick-cache — sculpt at more than one resolution

Delta for `add-multi-resolution`.

## ADDED Requirements

### Requirement: The cache states which level it holds
The brick cache is a sparse narrow band around one lattice, and that lattice is the one its `BrickConfig` names. It SHALL be unaffected by a voxel grid's resolution levels: it caches SDF evaluation, never voxel occupancy, so adding, dropping or selecting a voxel level SHALL neither dirty a brick nor invalidate a mip.

A voxel level and a brick mip are separate mechanisms for separate representations, and neither SHALL be implemented in terms of the other.

#### Scenario: A level change does not drop the cache
- **WHEN** a voxel grid's active level changes
- **THEN** every tracked brick keeps its generation and nothing is resubmitted
