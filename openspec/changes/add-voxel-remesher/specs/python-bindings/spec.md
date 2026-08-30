# python-bindings — voxel remesh

Delta for `add-voxel-remesher`.

## ADDED Requirements

### Requirement: Voxel remesh from Python
`pyclay` SHALL expose the global voxel remesh and its preflight estimate, taking resolution as either a longest-axis integer or a world voxel size, and returning the result mesh together with the report's fields as named values rather than as a positional tuple.

A refused remesh SHALL raise with a message naming which contract refused it — resolution, budget, open surface, validation or cancellation — rather than returning an empty mesh a caller has to diagnose.

The binding SHALL be covered by the repository's binding-parity check, so a parameter added to the C++ or C surface and not to Python is caught by the same gate every other surface is.

#### Scenario: A remesh runs from Python
- **WHEN** a mesh is remeshed from Python at a longest-axis resolution
- **THEN** a new mesh is returned whose triangle count differs from the source's, together with the resolved voxel size and the validation counts

#### Scenario: The estimate is reachable before the run
- **WHEN** the estimate is called from Python
- **THEN** it returns the resolved voxel size, the estimated memory and triangle range, and the open-boundary and component counts, without performing the remesh

#### Scenario: A refusal is an exception naming its cause
- **WHEN** a Python caller requests a remesh at an invalid resolution and one over a supplied memory budget
- **THEN** each raises, and the two messages name different causes
