# c-abi — curves

Delta for `add-curve-objects`.

## ADDED Requirements

### Requirement: Curves across the ABI
The C API SHALL accept control points with per-point radius, type and handles, a closed flag and a tolerance, and SHALL expose replacing a placed item's points.

#### Scenario: A curve means the same through both bindings
- **WHEN** a C consumer builds a curve with given control points, types and tolerance
- **THEN** the field matches what `pyclay` produces for the same curve
