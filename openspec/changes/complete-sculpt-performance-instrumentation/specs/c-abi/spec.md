## ADDED Requirements

### Requirement: A host can read the per-stage breakdown of a stamp

A host SHALL be able to read, across the C ABI, the per-stage times and counts
of the stamps a sculpting session has made, for the fixed mesh, the adaptive
surface and the multiresolution hierarchy alike.

**Without it a host has total dab time and nothing else**, which is the whole
of what this milestone exists to stop. The breakdown exists in C++ today and is
consumed by exactly one benchmark program; an engine that can measure something
its host cannot reach has the shape of defect this ABI has shipped before.

The report SHALL use a `struct_size`-prefixed descriptor carrying the times and
the counts together, so that a host reading "this stage got slower" can read
"because it touched more" in the same call.

Attaching a reader SHALL be explicit. A host that has not asked for the
breakdown SHALL pay nothing for it.

#### Scenario: A host reads a stamp's breakdown
- **WHEN** a host enables the breakdown on a sculpting session and makes a stamp
- **THEN** it can read the time and the work of each stage of that stamp

#### Scenario: A host that does not ask pays nothing
- **WHEN** a host makes a stamp without enabling the breakdown
- **THEN** no timing or counting is performed
