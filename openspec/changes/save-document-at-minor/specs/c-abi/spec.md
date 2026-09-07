## ADDED Requirements

### Requirement: A host can write a document at an older format layout

The C ABI SHALL be able to write a document at a scene format minor other than
this build's own, so that a host can hand a file to a build that has not been
updated.

The write SHALL be REFUSED rather than performed when that layout cannot say
what the document says, and the refusal SHALL name the first layer that blocks
it. It SHALL NOT return success with content that has been silently downgraded,
and it SHALL NOT return success with empty content.

The refusal SHALL be the same answer, from the same predicate, that the
compatibility query gives — so that asking before a save and being refused
during one cannot disagree.

The refusal SHALL name WHICH kind of thing blocked it, because a host told that
a layer carries a composition, about a layer that carries a hierarchy, will go
and look at the wrong thing.

**No new result code is added for this condition.** The existing "unsupported"
result already means it and already carries the blocking layer through the
query; a second code for one condition would be a second answer to one question,
and a host would have to handle both to be correct.

#### Scenario: A document that an older layout can say
- **WHEN** a document with nothing an older layout cannot express is saved at that layout
- **THEN** the write succeeds and produces a file that layout can open

#### Scenario: A document that an older layout cannot say
- **WHEN** a document carrying something an older layout has no way to express is saved at that layout
- **THEN** the write is refused, the first blocking layer is named, and no content is produced

#### Scenario: The query and the save agree
- **WHEN** the compatibility query refuses a layout for a document
- **THEN** a save at that layout is refused for the same layer

### Requirement: A refused save leaves what was there alone

A save that is refused SHALL NOT modify its destination. An existing file SHALL
be byte-identical afterwards.

A save that cannot represent the document must not first destroy the last one
that could — and the destination is often the artist's only copy.

#### Scenario: A refusal over an existing file
- **WHEN** a save at a layout that cannot express the document is attempted over an existing file
- **THEN** the call is refused and the file's bytes are unchanged

### Requirement: The compatibility query answers for the whole document

The query that reports whether a document can be written at a format minor
SHALL answer for everything the container carries, not for one field of it.

**It did not.** It asked only whether a layer's composition could be expressed,
because composition was the only field whose absence changed the model when the
query was written. A later minor added a chunk carrying a mesh layer's
multiresolution hierarchy, and the query did not learn about it: a document with
a hierarchy reported that an older minor could write it, and that minor has no
chunk to put one in — against a promise of "nothing an artist authored dropped".

Whether a thing blocks a write SHALL turn on whether it can be RECONSTRUCTED.
Something derived from what the file still carries, by a deterministic
computation, is a plainer file and is allowed to be absent. Something an artist
made is not, and refuses the write.

#### Scenario: A hierarchy holding only its cage
- **WHEN** a document whose hierarchy holds no more than its cage is checked against a minor that cannot carry hierarchies
- **THEN** the query reports it as writable, because the hierarchy rebuilds from the cage the file still carries

#### Scenario: A hierarchy an artist has worked in
- **WHEN** a document whose hierarchy carries levels above its cage is checked against a minor that cannot carry hierarchies
- **THEN** the query refuses and names the layer
