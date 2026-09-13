# brush-engine

## ADDED Requirements

### Requirement: The automask sources read the frame their own handle declares

The inputs that make `CLAY_AUTOMASK_CAVITY` and `CLAY_AUTOMASK_SURFACE_GROUP`
answer SHALL be asked about the point the surface actually occupies, on every
sculptor that can carry them.

#### Scenario: Each sculptor places its own sources

- **GIVEN** automask sources named through
  `clay_mesh_sculptor_set_automask_sources`,
  `clay_dynamic_sculptor_set_automask_sources` or
  `clay_multires_sculptor_set_automask_sources`
- **WHEN** either factor is consulted during a stamp
- **THEN** the cavity field and the group lattice SHALL be sampled at the point
  placed by the frame that sculptor declares

#### Scenario: The frame is read when the lattice is asked

- **GIVEN** a host that names its sources BEFORE declaring its frame
- **WHEN** it then declares one and stamps
- **THEN** the lattices SHALL be asked at the placed point, because the frame is
  read at the moment the lattice is consulted and not captured when the sources
  were named

#### Scenario: One transform, not two

- **GIVEN** a host that both names sources through the C setter and drives a
  stroke that carries its own cavity field
- **WHEN** the second set of inputs is installed
- **THEN** it SHALL REPLACE the first rather than compose with it, so the point
  is placed once and not twice
