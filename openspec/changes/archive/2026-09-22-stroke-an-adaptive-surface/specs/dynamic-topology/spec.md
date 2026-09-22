## ADDED Requirements

### Requirement: A stroke keeps the per-verb remesh schedule on every stamp
An adaptive stroke SHALL run the remesher around EVERY stamp at the timing the verb's default records — after the deformation for Grab, before it for the deposit family including Clay, before and after it for Snakehook — exactly as a single stamp of that verb does. A stroke SHALL NOT substitute a stroke-level schedule, such as one remesh per several stamps, for the per-verb one.

The schedule stays one function with one reason per verb, and a stroke's topology stays a pure function of its stamps: the same stamps applied one at a time produce the same surface.

A verb the adaptive surface does not offer SHALL be refused for a whole stroke as it is for one stamp, before any remesh runs.

#### Scenario: A stroke's topology is its stamps' topology
- **WHEN** the same resolved stamps are applied as one adaptive stroke and as a sequence of single stamps, for a verb of each timing
- **THEN** the split, collapse and flip counts agree and the surfaces are bit-identical

#### Scenario: A refused verb runs no remesh
- **WHEN** a Layer stroke is applied to an adaptive surface with topology enabled
- **THEN** no split, collapse or flip runs and the topology revision is unchanged
