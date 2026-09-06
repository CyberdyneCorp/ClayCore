## ADDED Requirements

### Requirement: A document says when a command has changed it

A document SHALL carry a serial that advances whenever a command changes it, so
that a consumer can tell the document before an edit from the document after
one.

It SHALL advance where commands are APPLIED rather than where a binding
invalidates its caches. Those are different moments: a binding invalidates once
an edit is finished, which is after both sides of that edit have been examined,
so a consumer keyed on the later moment cannot distinguish them and will answer
a question about the new document with the old one's geometry. A bound derived
that way is too small, and a bound that is too small is under-invalidation —
which renders as stale geometry rather than as an error.

It SHALL advance for every command-based mutation without those mutations being
enumerated, so that undo, redo and a replayed journal are covered by
construction and a command added later cannot be forgotten.

It SHALL NOT advance for a command that was refused, since such a command leaves
the document unchanged.

A mutation made outside the command vocabulary does not reach that point, and
the component owning such a path SHALL advance the serial where it already
declares its caches stale.

#### Scenario: The two sides of an edit are distinguishable
- **WHEN** a value derived from the document is taken before a command is applied and again after it
- **THEN** the serial differs between the two, so a cache keyed on it cannot answer the second with the first

#### Scenario: Undo is covered without being named
- **WHEN** a document is changed and then undone
- **THEN** the serial advances for the undo as it did for the edit, because both apply a command

#### Scenario: A refused command changes nothing
- **WHEN** a command is refused, for instance against a protected layer
- **THEN** the serial does not advance
