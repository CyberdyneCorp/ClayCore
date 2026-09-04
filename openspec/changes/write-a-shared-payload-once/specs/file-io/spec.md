## ADDED Requirements

### Requirement: A payload several items share is stored once

Where several items hold ONE sampled payload — a sampled volume, or the mask
that gates an item — the document SHALL store it once and every holder SHALL
name it. Storing a copy per item makes a document that instances one asset grow
with the number of instances rather than with what it contains, which is the
difference between a reusable asset and one that can be used a handful of times.

**A reload SHALL preserve the sharing.** Deduplicating on write and rebuilding a
separate payload per item on read saves space in the file and restores the
duplication in memory, and the next save writes the copies again — so the two
halves are one requirement and not two.

The name a holder uses SHALL be unambiguous across the whole document. An
identifier that is only unique within a layer names a different item in another
layer, and the document model numbers each layer's items from one.

Sharing SHALL be identity, not equality: two payloads with equal contents that
were built separately are two payloads, exactly as two identical edit lists are.
Deduplicating by content would make the file's structure depend on a comparison
of megabytes and would silently merge two assets an artist may edit apart.

A document written for an older reader SHALL fall back to storing a payload per
item, so that reader opens it and gets what it always got. What such a downgrade
costs SHALL be size alone, never content.

#### Scenario: One asset placed many times
- **WHEN** a document places one captured payload many times and is saved
- **THEN** its size grows by a small record per placement rather than by the payload, and the payload appears once

#### Scenario: The sharing survives a reload
- **WHEN** such a document is loaded and saved again
- **THEN** every placement still refers to one payload, and the second file is the same size as the first

#### Scenario: An older reader still opens it
- **WHEN** the document is written for a reader that predates shared payloads
- **THEN** each item carries its own copy, the reader opens it, and the field every item contributes is unchanged
