# file-io — persisting an adaptive surface

Delta for `add-dynamic-topology`.

## ADDED Requirements

### Requirement: An adaptive surface has its own versioned encoding
An adaptive surface SHALL serialize through its own versioned encoding carrying a magic, a version, validated counts, overflow-checked sizes and the attribute channels present. It SHALL NOT be written into the existing mesh stream, whose readers expect flat interchange arrays.

A decoder SHALL reject hostile or truncated counts BEFORE allocating, following the defensive style the sparse vertex delta decoder already uses.

The document format SHALL remain BACKWARD-OPEN: a reader that predates this chunk SHALL open the document without it rather than failing, and a document containing no adaptive surface SHALL be byte-identical to one written before this change.

A round trip SHALL restore the surface exactly. Where an identity that cannot survive a round trip is not preserved — a generation counter, say — the format SHALL state that rather than imply preservation it does not provide.

#### Scenario: A round trip preserves the surface
- **WHEN** a document holding an adaptive surface is saved and reloaded
- **THEN** the surface's geometry, connectivity and attributes are restored exactly

#### Scenario: An older reader is not broken
- **WHEN** a reader that predates this chunk opens a document containing one
- **THEN** it opens the document without the adaptive surface rather than reporting a corrupt file

#### Scenario: A truncated stream is refused
- **WHEN** a stream declares counts larger than its own remaining bytes
- **THEN** the decode fails before allocating and reports a typed error
