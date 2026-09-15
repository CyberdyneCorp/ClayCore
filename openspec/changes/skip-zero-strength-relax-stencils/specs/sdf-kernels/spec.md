## ADDED Requirements

### Requirement: Zero-strength relax avoids neighborhood sampling
When relax strength clamps to zero, the library SHALL preserve stored sample bits
without constructing or reading a smoothing stencil or evaluating the mask.
It SHALL retain geometric selected-brick reporting, existing completed-pass band
handling, and cancellation at pass boundaries. A transaction update SHALL still
materialize its requested source region and provide the corresponding preview delta.

#### Scenario: Zero or negative strength
- **WHEN** whole-volume or regional relax runs with zero or negative strength
- **THEN** no mask or stencil samples are evaluated
- **AND** stored samples remain bit-identical and the selected region is reported

#### Scenario: Preview priming
- **WHEN** a fresh Smooth transaction receives a whole-field zero-strength update
- **THEN** its full requested source field is materialized and available as a preview delta
- **AND** no persistent document edit is recorded

#### Scenario: Cancellation before a pass
- **WHEN** a zero-strength relax call receives an already-cancelled token
- **THEN** it reports cancellation and leaves both samples and band unchanged

#### Scenario: Nonzero strength
- **WHEN** clamped strength is nonzero
- **THEN** existing smoothing arithmetic and mask behavior are preserved
