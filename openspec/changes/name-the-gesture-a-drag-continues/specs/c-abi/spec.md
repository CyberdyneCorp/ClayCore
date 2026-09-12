## ADDED Requirements

### Requirement: A host can name the gesture a drag belongs to

A surface drag SHALL accept an identifier naming the gesture it belongs to, and
SHALL treat two drags carrying the same identifier as one gesture however their
centre or radius moved between them.

A drag that continues a gesture replaces what that gesture already emitted
rather than stacking on it. Deciding which grabs those are by comparing the
drag's centre and radius exactly is correct only for a gesture that holds both
fixed; a radius driven by pen pressure, a centre that follows the pointer, or
either recomputed through a different arithmetic path produces a new key every
frame and the replacement stops happening. The cost is linear in the length of
the gesture.

An identifier SHALL NOT be matched approximately. Two gestures whose centres
are merely close are two gestures, and folding them would change the document
rather than its cost.

An unset identifier SHALL preserve the previous behaviour exactly, so that a
caller that does not name its gestures is unaffected. A named gesture SHALL NOT
continue an unnamed one, nor one carrying a different name.

The identifier SHALL NOT be persisted. It describes a gesture in progress; a
stored document holds the finished result, and on loading one the unset
behaviour applies.

#### Scenario: A drag whose radius follows pressure stays one gesture
- **WHEN** a drag is applied over many frames with a changing radius and one identifier
- **THEN** it costs what an anchored drag of the same length costs

#### Scenario: A drag whose centre follows the pointer stays one gesture
- **WHEN** a drag is applied over many frames with a moving centre and one identifier
- **THEN** it costs what an anchored drag of the same length costs

#### Scenario: Two gestures stay two
- **WHEN** two drags carrying different identifiers are applied in turn
- **THEN** the second does not replace what the first emitted

#### Scenario: An unnamed gesture is unaffected
- **WHEN** a drag is applied without an identifier
- **THEN** it behaves exactly as it did before identifiers existed
