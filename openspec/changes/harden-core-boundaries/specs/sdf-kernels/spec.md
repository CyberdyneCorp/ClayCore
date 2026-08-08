# sdf-kernels — a record that was never written is never read

Delta for `harden-core-boundaries`.

## ADDED Requirements

### Requirement: A loft with fewer than two profiles evaluates far
The loft opcode SHALL return the far value when its profile count is below two, rather than reading the two records it interpolates between. A tape is rebuilt from a document on every compile and a document may come from disk, so the count is not something the authoring layer alone can guarantee.

This is the guard the sweep opcode already applies to its own guide and profile counts.

#### Scenario: A loft carrying no profiles
- **WHEN** a document whose loft node has zero profiles is compiled and evaluated
- **THEN** evaluation returns a finite far value and reads no record

#### Scenario: A loft carrying one profile
- **WHEN** a document whose loft node has a single profile is compiled and evaluated
- **THEN** evaluation returns a finite far value and reads no record beyond that profile

#### Scenario: A well-formed loft is unaffected
- **WHEN** a loft with two or more profiles is evaluated
- **THEN** it interpolates between them exactly as before

### Requirement: A sampled volume's brick index is bounded by its sample data
`FieldVolume::from_blob` SHALL reject a blob in which any brick index entry is neither the empty marker nor an offset from which a whole brick of samples lies inside the sample section. Checking only the section offsets leaves one entry able to name an arbitrary offset, which the evaluator and the tape both then read a full brick from.

#### Scenario: An index entry pointing past the samples
- **WHEN** a volume blob carries a brick index entry whose offset plus one brick runs past the sample data
- **THEN** the blob is refused

#### Scenario: A negative entry that is not the empty marker
- **WHEN** a volume blob carries a negative brick index entry other than the empty marker
- **THEN** the blob is refused

#### Scenario: A sparse volume still loads
- **WHEN** an ordinary volume with empty bricks is round-tripped through its blob
- **THEN** it loads and evaluates unchanged
