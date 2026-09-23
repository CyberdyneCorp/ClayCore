## ADDED Requirements

### Requirement: The mixed-depth export is reachable across the boundary

A hierarchy's mixed-depth export SHALL be reachable through the C ABI, as one
watertight mesh and one base patch at a time, with the same guarantees the
engine gives it: watertight by identity, a quad list only where no edge is
split, and a refusal reported by name rather than as a partial result.

The per-patch form SHALL NOT answer success with an empty block for a patch it
cannot present. Buffers SHALL be the caller's, a short buffer SHALL be reported
as `CLAY_ERROR_BUFFER_TOO_SMALL`, and every descriptor SHALL start with a
`uint32_t struct_size` and grow by appending.

#### Scenario: A host assembles a watertight mixed-depth mesh through the ABI
- **WHEN** a host exports a regionally refined hierarchy at a display level above its coarsest depth through the C ABI
- **THEN** the mesh it receives has no open edges, where assembling the per-patch blocks at each patch's effective level leaves open edges at every depth boundary

#### Scenario: A split cage is refused by name
- **WHEN** a host asks for attributes on a cage whose attribute connectivity is split
- **THEN** the call fails with a named refusal and writes no partial mesh
