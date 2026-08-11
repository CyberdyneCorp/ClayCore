## ADDED Requirements

### Requirement: The kernel dialect provides a feathered replace of a sampled volume
The combine vocabulary SHALL include a feathered replace, emitted by the tape COMPILER when a volume item carrying a feather is placed with Replace — not a public op, and never emitted for a feather of zero, so every existing tape is bit-identical. The mode SHALL crossfade the accumulated field to the volume over the feather margin just inside the sampled box, weighting by the inset into the box, and SHALL read the box, band and feather from the same volume blob header the volume primitive reads, so the two cannot disagree.

The correction SHALL be clamped at the volume's band. That clamp is load-bearing twice: the declared Lipschitz stays closed-form — the operands' bound plus band times the weight's peak slope over the feather (`cfi_replace_feather`) — and per-brick culling keeps the CullRegion contract, PROVIDED the compiler widens its cull test by the same band whenever a feathered replace is present, which it SHALL do. A chain the cull emptied SHALL keep the feathered blend against the far-field seed, while a chain that is truly empty SHALL degrade to the hard replace, so a lone feathered volume shows its shape and every per-brick tape stays band-clamp bit-identical to the full one.

#### Scenario: Every backend agrees on the feathered field
- **WHEN** a document containing a feathered replace is evaluated on any registered backend
- **THEN** the results match the shared interpreter exactly, because the mode lives in the one tape dialect every backend compiles

#### Scenario: Per-brick culling stays exact under the feather
- **WHEN** per-brick tapes of a document containing a feathered replace are compiled with a cull region and compared against the full tape inside that region
- **THEN** band-clamped results are bit-identical, including bricks whose chains the cull emptied and documents where the volume is the only item

### Requirement: The volume blob header carries the feather by its self-describing rule
The volume blob header SHALL append the feather after the sample Lipschitz, growing the header from 12 to 13 floats under the existing rule that the header's size IS the index offset. A blob written before the field SHALL read as feather zero — the hard replace — and a reader written before the field SHALL find its section offsets exactly where they always were.

#### Scenario: Round trips carry it and old blobs mean hard
- **WHEN** a volume with a feather is taken to a blob or serialized bytes and back
- **THEN** the feather survives; and a blob whose header predates the field reads back with feather zero and identical samples
