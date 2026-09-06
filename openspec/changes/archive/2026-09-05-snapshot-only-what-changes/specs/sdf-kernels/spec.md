# sdf-kernels — snapshot only what a pass will overwrite

Delta for `snapshot-only-what-changes`.

## ADDED Requirements

### Requirement: An operator reads its input without copying the field
An operator whose result at a sample depends on that sample's NEIGHBOURS needs the pass's input rather than its half-written output, and SHALL obtain one without duplicating the whole field. Copying it costs what the model costs — megabytes at an interactive cell to protect the few hundred kilobytes a brush touches — which puts back into a dab the very term that confining the traversal to a region took out.

Preserving only the samples the pass will overwrite SHALL be sufficient, and it is sufficient for the same reason the region limit is sound: a brick the operator does not write still holds what it held, so it can answer for itself. It follows that the preserved set and the written set MUST be the same set; preserving less means reading bricks the pass is part way through, and that is a silent wrong answer rather than a refusal.

Reading a preserved sample SHALL cost no more than reading it from the field would have. A stencil asks several times per sample and a dab covers tens of thousands of samples, so a lookup that is slower per tap loses more than the copy saves however much smaller it is.

#### Scenario: What was preserved reads as it was
- **GIVEN** a snapshot of a region, taken before that region is rewritten
- **WHEN** the region is rewritten and the snapshot is read
- **THEN** every sample it reports is the value the field held before the rewrite began — inside the region and outside it alike

#### Scenario: A sample shared across the preserved boundary
- **WHEN** a sample lying on the face between a preserved brick and one that was not preserved is read from the snapshot
- **THEN** it reports the value that sample held before the rewrite

#### Scenario: A dab costs what it moves
- **GIVEN** the same brush applied to two volumes of the same surface, one covering far more of it
- **WHEN** a stroke of dabs is applied to each
- **THEN** neither the traversal, nor the bounds it re-derives, nor what it preserves to read from scales with the field rather than the brush
