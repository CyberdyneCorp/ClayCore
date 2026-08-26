# file-io — scale an item per axis

Delta for `scale-an-item-per-axis`.

## ADDED Requirements

### Requirement: The per-axis scale is a gated appended field
The node record SHALL carry an item's per-axis scale from scene minor 14, appended after the fields the record already held rather than placed beside the transform, so that a build predating the field reads exactly the bytes it always did.

Writing a document AT minor 13 or below SHALL omit it, and the item SHALL then degrade to its UNIFORM scale — a squashed cylinder comes back round rather than missing. That is the recoverable direction and the one an older build can evaluate, and it SHALL be documented at the constant rather than left to be discovered.

Reading a document written at minor 13 or below SHALL leave the per-axis scale at its default of `(1, 1, 1)`, which is exactly what those documents already meant, so an older file's field is unchanged rather than reinterpreted.

The transform COMMAND SHALL carry the same three floats under the same gate, because that command carries the whole transform and a journal written at an older minor must replay on a build that predates the field as the uniform transform it always was.

#### Scenario: A squash round trips
- **WHEN** a document containing an item with a per-axis scale is serialized and read back
- **THEN** the scale is exactly what was written, and reserializing produces identical bytes

#### Scenario: An older minor drops it and keeps everything else
- **WHEN** the same document is serialized at minor 13
- **THEN** the bytes are identical to those of the same document with no per-axis scale, and reading them back gives an item with the default per-axis scale and its uniform scale intact
