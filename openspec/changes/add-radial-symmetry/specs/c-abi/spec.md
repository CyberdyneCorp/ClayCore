# c-abi

## ADDED Requirements

### Requirement: A host can set and clear a layer's radial symmetry
The C ABI SHALL expose setting a layer's radial symmetry by count, axis and seam blend, and SHALL treat a count of 0 or 1 as clearing it. The call SHALL respect a locked layer and SHALL be undoable, matching the layer mirror rather than writing the field directly — the defect the mirror entry point was created to fix.

An axis outside 0..2, or a negative blend, SHALL be rejected with an invalid-argument result rather than clamped.

#### Scenario: Setting and clearing round-trips
- **WHEN** a host sets a radial count of 8 and then sets 0
- **THEN** both calls succeed, the second restores the un-arrayed field, and each is a separate undo step

#### Scenario: A locked layer refuses
- **WHEN** a host sets a radial count on a locked layer
- **THEN** the call fails and the layer is unchanged

### Requirement: Radial symmetry survives a document round-trip
A document written and read back SHALL preserve a layer's radial count, axis and seam blend. A document written by a build that predates the field SHALL load with the mode off rather than failing.

#### Scenario: Save and load preserves the mode
- **WHEN** a document with a radial layer is serialized and read back
- **THEN** the layer's count, axis and blend match, and the field evaluates identically

#### Scenario: An older document loads with it off
- **WHEN** a document written before this field existed is read
- **THEN** it loads successfully with a radial count of 0
