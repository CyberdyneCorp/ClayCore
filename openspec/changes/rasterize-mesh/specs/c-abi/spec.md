# c-abi — a host rasterizes a mesh

Delta for `rasterize-mesh`.

## ADDED Requirements

### Requirement: Rasterizing a mesh across the ABI
The C ABI SHALL expose mesh rasterization onto a voxel grid, taking a mesh handle and an OPTIONAL region.

Both region pointers SHALL be given or neither, as elsewhere in this ABI, and a region that is not finite, is empty, or is unbounded SHALL be REFUSED before the grid is touched — so a rejected call leaves the grid as it was rather than half-rasterized, exactly as the document rasterizer already guarantees.

A NULL region SHALL mean the mesh's own bounds rather than being an error, which is the one place this entry point differs from `clay_voxel_rasterize`: a document may have no bounded content and a mesh always does.

A mesh with no triangles SHALL be refused with a typed error rather than silently doing nothing, because a caller that loaded a file and got nothing wants to know.

The header SHALL carry the same statement of what the sampling preserves that the document rasterizer carries, and SHALL state that this is occupancy sampling and not retopology.

#### Scenario: A host rasterizes an imported model
- **WHEN** a host loads an OBJ and rasterizes it with a NULL region
- **THEN** the grid is filled over the mesh's own bounds and the call returns `CLAY_OK`

#### Scenario: A malformed region is refused and changes nothing
- **WHEN** a host passes one region pointer, or a region carrying a non-finite bound
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the grid is still empty
