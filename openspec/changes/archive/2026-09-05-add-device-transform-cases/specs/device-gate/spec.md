# device-gate

## ADDED Requirements

### Requirement: A drag frame is a measured unit

A gizmo drag SHALL be measured, and its timed unit SHALL be ONE FRAME of the
drag: apply the placement, dirty what it reached, and refill — the same
edit-then-display unit every other interactive case uses, because a host cannot
draw until it has evaluated the result.

A drag case SHALL NOT reset between iterations. A drag has no chain for a reset
to protect: each frame's placement supersedes the last, and the document stays
the size the growth axis names however many frames run. This is the opposite of
the stamp cases, whose reset exists precisely because a stamp grows the
document, and stating it keeps a later reader from adding one for symmetry.

The placement a drag case applies SHALL WALK, and SHALL NOT alternate between
two values. Consecutive frames of a real drag land near each other and dirty
overlapping regions; a two-point flip dirties the same region twice and can be
served from what the previous frame left, so it measures a resume the artist
never gets.

A drag case SHALL record how many bricks one frame refilled, for the same
reason the stamp case does: it separates "the invalidation is too wide" from
"each brick is expensive", which are different defects with different fixes.

#### Scenario: A drag case takes no reset
- **WHEN** a drag case runs its growth axis
- **THEN** the document holds the same number of items at the end of the case as at the start, with no reset having run

#### Scenario: The placement walks
- **WHEN** the placements a drag case applies across one point of its axis are collected
- **THEN** no placement repeats, and consecutive placements are near enough that their influence bounds overlap

#### Scenario: A frame that refilled nothing is a failure
- **WHEN** every timed iteration of a drag case refills zero bricks
- **THEN** the case fails rather than reporting the cost of an empty loop

### Requirement: The transform verbs are covered or exempt

The verbs that place a node or a layer SHALL be part of what the coverage
checker knows about, so a transform entry point without a device case is an
error rather than an absence nobody can see. The checker's verb patterns SHALL
match the placement entry points, and the coverage table SHALL carry an entry —
measured or explicitly exempt — for each.

A placement case SHALL be recorded for a node at the layer ROOT and for a node
inside a GROUP separately. They exercise the same entry point and cost
differently by a factor a gate can hold, because what an edit invalidates is
derived from the node's ancestry rather than from the node; one case standing
for both would report whichever the fixture happened to build.

#### Scenario: A transform verb without a case is named
- **WHEN** a placement entry point exists in the header and the coverage table has no entry for it
- **THEN** the coverage checker fails naming that verb

#### Scenario: Grouped and ungrouped are separate rows
- **WHEN** the run record is read
- **THEN** it carries a node-placement case whose dragged node is at the layer root and another whose dragged node is inside a group, under different case names
