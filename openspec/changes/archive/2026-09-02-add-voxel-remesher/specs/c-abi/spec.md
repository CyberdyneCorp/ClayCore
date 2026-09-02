# c-abi — voxel remesh

Delta for `add-voxel-remesher`.

## ADDED Requirements

### Requirement: Voxel remesh over the C ABI
The C ABI SHALL expose the global voxel remesh and its preflight estimate, taking a versioned `struct_size`-prefixed parameter descriptor and filling versioned `struct_size`-prefixed estimate and report descriptors.

Every descriptor the library FILLS SHALL be written bounded by the size the caller declared, never by the size this build compiled, so a caller built against an older header is not written past. A defaults accessor SHALL be provided for the parameter descriptor so a caller can obtain the library's documented defaults without transcribing them.

The remesh SHALL accept the ABI's existing cancellation token, so a host can drive it from a worker thread and stop it, and SHALL report a cancelled call as a distinct result code rather than as a generic failure.

Failure SHALL be distinguishable by kind: an invalid resolution, a request over budget, an open surface refused by policy, a result that failed its own validation and a cancellation SHALL NOT collapse into one code.

#### Scenario: An older caller is not written past
- **WHEN** a caller declares an estimate or report descriptor shorter than this build's
- **THEN** only the bytes the caller declared are written, and the fields the caller does know are correct

#### Scenario: Defaults are obtainable, not transcribed
- **WHEN** a caller asks for the voxel remesh parameter defaults
- **THEN** it receives a filled descriptor whose values match the C++ defaults, and a remesh with it behaves as a remesh with the C++ defaults

#### Scenario: Failure kinds stay distinct
- **WHEN** a remesh is refused for an invalid resolution, for exceeding a budget, and for an open surface under a rejecting policy
- **THEN** the three calls return three different result codes

#### Scenario: A cancelled remesh is reported as cancelled
- **WHEN** a remesh is cancelled through the ABI's cancellation token
- **THEN** the call returns the cancelled result code and produces no mesh

### Requirement: Spatial scalar transfer over the C ABI
The C ABI SHALL expose the spatial resampling of a caller-owned per-vertex scalar array from one mesh onto another, so a host holding a mask outside the mesh can carry it across a remesh.

The call SHALL require the caller to state the length of both the input array and the output buffer, and SHALL refuse a length that does not match the corresponding mesh's vertex count rather than reading or writing what it was not given.

#### Scenario: A mask crosses a remesh
- **WHEN** a host transfers a per-vertex mask from a source mesh onto its remeshed result
- **THEN** the output buffer holds one value per result vertex, resampled from the source by closest point

#### Scenario: A wrong length is refused
- **WHEN** the call is given an array whose length is not the source's vertex count
- **THEN** it returns an invalid-argument result and writes nothing
