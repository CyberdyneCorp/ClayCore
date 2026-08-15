# voxel-engine — move a sculpt between the two representations

Delta for `add-representation-round-trip`.

## ADDED Requirements

### Requirement: A grid converts directly to a field
The module SHALL convert a voxel grid to a narrow-band signed distance field without going through a mesh, and the result SHALL carry a Lipschitz bound good enough for sphere tracing rather than the step function the grid samples today.

The palette SHALL survive the conversion.

#### Scenario: A round trip preserves the surface within tolerance
- **WHEN** a shape is rasterised to a grid and converted back to a field
- **THEN** the two surfaces agree within the stated tolerance for the cell size used

#### Scenario: Colour survives
- **WHEN** a grid with several palette entries is converted
- **THEN** the colours are present in the result

### Requirement: Conversion is lossy, and says so
Conversion SHALL be specified as a conversion rather than a view. Going to a grid quantises to the lattice, so a boolean's sharp edge becomes a staircase at the cell size and is not recoverable; going to a field turns binary occupancy into a distance whose meaning is bounded by the band width.

The procedural history SHALL be stated as lost on conversion — once converted, the items are no longer editable as parameters.

#### Scenario: A sharp edge degrades to the cell size
- **WHEN** a boolean with a sharp edge is rasterised and converted back
- **THEN** the edge is rounded at the scale of the cell size, and this is the documented outcome rather than a defect
