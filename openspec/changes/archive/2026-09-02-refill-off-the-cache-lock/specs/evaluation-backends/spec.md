# evaluation-backends

## ADDED Requirements

### Requirement: A seeded walk may run in place

`eval::eval_points_seeded` SHALL accept a seed buffer that IS its destination,
for the distances and for the colours alike. It reads a block's seed into its
stack before it writes that block's result, so aliasing the two is exact rather
than merely tolerated.

This is what lets a caller copy a seed out of wherever it is kept into the
buffer it is about to fill, and pay one `memcpy` rather than a second buffer and
a second pass.

#### Scenario: seed and destination are the same buffer

- **GIVEN** a suffix tape and a seed
- **WHEN** the walk is asked to write its result over the seed it was given
- **THEN** the result is the one it gives when the two are separate buffers,
  bit-for-bit
