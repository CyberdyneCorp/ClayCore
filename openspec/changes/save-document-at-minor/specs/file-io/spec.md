## ADDED Requirements

### Requirement: The container can be written at an older minor

Saving the container SHALL accept the format minor to write at, defaulting to
this build's own.

**A chunk a minor did not have SHALL NOT be written at that minor.** A file
claiming an older minor while carrying a chunk that minor never defined is a
file whose version says one thing and whose bytes say another, and the reader
that trusts the version is the one that breaks.

The write SHALL be refused — returning no bytes, which is never a valid stream —
when the requested minor cannot express the document. This is the convention the
scene payload's own serializer already follows, so a caller handling one handles
both.

A file write SHALL leave its destination untouched on a refusal.

#### Scenario: A chunk introduced by a later minor
- **WHEN** a document carrying a chunk a later minor introduced is written at a minor that predates it
- **THEN** either the chunk is absent because nothing an artist made is in it, or the write is refused

#### Scenario: A refusal produces no stream
- **WHEN** the requested minor cannot express the document
- **THEN** no bytes are produced, rather than a shorter stream that opens

### Requirement: One query answers whether a document can be written at a minor

There SHALL be a single query reporting the first layer, of any kind, that a
format minor cannot express.

Per-field queries each know about one thing, and a caller that had to ask
several and combine them is the place they get out of step — which is exactly
how a hierarchy came to be missing from an answer that promised to cover
everything an artist authored.

#### Scenario: Two kinds of blocker, one answer
- **WHEN** a document carries both a layer composition and a hierarchy that a minor cannot express
- **THEN** one query reports a blocking layer, and the caller does not have to know how many kinds of blocker exist
