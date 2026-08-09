# c-abi — Read Curve Points

Delta for `read-curve-points`.

## MODIFIED Requirements

### Requirement: Curves across the ABI
The C API SHALL accept control points with per-point radius, type and handles, a closed flag and a tolerance, and SHALL expose replacing a placed item's points and READING them back. The readback SHALL take the same arguments as the replace, with the count as an in/out pointer, and SHALL follow the size-query convention: a null point buffer answers with the count, an undersized buffer yields a too-small error carrying the needed count and writes nothing, and the optional parallel arrays are each independently omittable. The points SHALL come back as authored rather than tessellated, so that feeding a readback into the replace leaves the document unchanged. A guide belonging to a swept item SHALL be readable and replaceable through the same calls, since a guide is an ordinary curve, and the replace SHALL enforce on it the same rules placing a swept item enforces: a guide SHALL NOT be closed, and SHALL NOT be left with fewer than two points. Reading SHALL NOT be refused on a protected or hidden layer, because protection refuses edits.

#### Scenario: A curve means the same through both bindings
- **WHEN** a C consumer builds a curve with given control points, types and tolerance
- **THEN** the field matches what `pyclay` produces for the same curve

#### Scenario: A reloaded document's curve round trips
- **WHEN** a host reads a placed curve's points back and feeds them straight into the replace
- **THEN** every point, type and handle comes back unchanged and the field is what it was

#### Scenario: A profiled tube's guide is reachable
- **WHEN** a host reads back a node built by the tube call with a profile
- **THEN** it receives the guide's control points, and replacing them reshapes the tube

#### Scenario: A guide keeps the rules its item was placed under
- **WHEN** the replace would close a swept item's guide, or leave it under two points
- **THEN** it is refused with the reason, and the guide is what it was

#### Scenario: A protected layer still answers a read
- **WHEN** a curve on a ghosted or locked layer is read back
- **THEN** the points are returned, while an edit to the same curve is still refused

#### Scenario: A short buffer says what it needed
- **WHEN** a host asks for the points with a buffer smaller than the point count
- **THEN** the call reports too-small, writes nothing, and leaves the needed count in the count argument
