# sdf-kernels — a dab costs what the dab adds

Delta for `seed-a-suffix-tape`.

## ADDED Requirements

### Requirement: An appended edit can be evaluated without replaying the chain
An edit list SHALL be evaluable from the value its unchanged prefix produced, rather than by re-running that prefix. A brush stamp appends one item to a list of thousands, and re-evaluating the thousands over a dirty brick's samples is what makes a dab cost what the document holds instead of what the dab adds: measured at a 0.05 voxel, one dab into twelve bricks is 0.23 ms against 200 items and 18.07 ms against 50,000, with per-brick culling working on both.

Continuing from that value SHALL be EXACT, not an approximation. The compiler emits each item's contribution as a self-contained expression and folds it into one running accumulator, so after every item the stack holds exactly one value — including under a layer mirror, where an item emits two primitives and a combine before folding in. Continuing therefore runs the same instructions in the same order over the same floats, with the part already folded represented by the number it produced, and the result SHALL be bit-identical to evaluating the whole document. A tolerance SHALL NOT be accepted in its place: the two are the same arithmetic, so anything short of identity means the suffix is not the suffix.

A suffix SHALL be refused wherever the compiler cannot be certain the prefix still describes the document — no checkpoint, the layer gone or no longer the one an append extends, or the appended items not actually at the tail of its list. A refusal costs the full evaluation the caller would have paid anyway; a wrong reuse is silent.

A suffix tape SHALL NOT be evaluable as an ordinary tape. It holds only the appended items and its bounds describe only them, so an evaluator whose stack starts empty would compute the suffix against empty space rather than against the shape.

#### Scenario: A seeded suffix is the whole document
- **GIVEN** a document whose last few items were appended to a chain
- **WHEN** only those items are compiled and evaluated from the value the rest produced
- **THEN** every point is bit-identical to evaluating the whole document

#### Scenario: The saving follows the dab rather than the document
- **WHEN** the same appended item is evaluated against documents of very different sizes
- **THEN** what it costs is set by the item rather than by how much history precedes it

#### Scenario: A suffix of nothing is the value it was given
- **WHEN** nothing was appended
- **THEN** evaluation returns the seed, rather than the "far outside" an empty tape means to an empty stack

#### Scenario: An uncertain prefix is refused
- **WHEN** the items claimed as appended are not the tail of the layer they name, or the checkpoint names a layer that is no longer there
- **THEN** the compile is refused and the caller evaluates in full
