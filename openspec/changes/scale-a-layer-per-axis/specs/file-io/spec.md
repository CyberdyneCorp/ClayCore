# file-io

## ADDED Requirements

### Requirement: The layer scale and the cage map are versioned

The scene and container minors SHALL move together for the layer's per-axis
scale and the lattice cage's affine map, and both fields SHALL be gated on the
writer's minor exactly as the radial fields and the shared-content id are: a
stream written at an older minor SHALL NOT carry fields that minor's reader will
not consume.

A stream written before this minor SHALL load with the layer scale at
(1, 1, 1) and the cage map at the rigid placement it recorded — both of which
are what those files always meant — rather than failing.

#### Scenario: An older document loads unchanged
- **WHEN** a document written at the previous minor is read
- **THEN** every layer carries a per-axis scale of ones, every transformed lattice carries the map its rigid placement described, and the document evaluates to what it evaluated to before

#### Scenario: A squashed layer round-trips
- **WHEN** a document holding a per-axis-scaled layer is written and read back
- **THEN** the three factors return exactly and the reloaded document evaluates bit-identically

#### Scenario: An older writer does not emit the new fields
- **WHEN** a document is written at a minor below this one
- **THEN** neither field is emitted and a reader at that minor consumes the stream without desynchronising
