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

### Requirement: A round cone whose ends contain one another is the larger sphere
When the radii of a round cone differ by more than the distance between its ends, one end sphere contains the other and no conical flank exists. Both the origin-and-height form and the endpoint form SHALL return the enclosing sphere's distance rather than evaluating the flank, which takes the square root of a negative quantity and yields NaN.

NaN is not a local error here: every combine op propagates it, so one such item makes the entire document evaluate to NaN, and a document carrying one round-trips through a file without complaint.

The endpoint form is the stroke-segment kernel, so this is reached by an ordinary tapered stroke, and its guard SHALL also cover coincident endpoints.

#### Scenario: A cone whose base swallows its tip
- **WHEN** a round cone is evaluated with a base radius exceeding its tip radius by more than its height
- **THEN** the result is finite and equals the distance to the base sphere

#### Scenario: A cone whose tip swallows its base
- **WHEN** the tip radius exceeds the base radius by more than the height
- **THEN** the result is finite and equals the distance to the tip sphere

#### Scenario: A stroke segment with coincident points
- **WHEN** the endpoint form is evaluated with both endpoints at the same position
- **THEN** the result is finite and equals the distance to the sphere at that point

#### Scenario: A well-formed cone is unchanged
- **WHEN** a round cone whose radii differ by less than its height is evaluated
- **THEN** it returns exactly what it returned before

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

#### Scenario: A volume larger than float can index exactly still loads
- **WHEN** a volume whose sample data exceeds what a float can address in consecutive integers is round-tripped through its blob
- **THEN** each entry is resolved to the brick boundary it names and the volume loads and evaluates unchanged

The check SHALL NOT be tighter than the format's own precision: a bound that assumes exact float integers refuses the last brick of any volume large enough to lose them, which makes a document this library wrote unopenable.
