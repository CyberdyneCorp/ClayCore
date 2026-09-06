# c-abi — a resumed refill spans layers

Delta for `resume-across-layers`.

## MODIFIED Requirements

### Requirement: A brick refill continues from its own previous result
Refilling a brick SHALL evaluate only what the document gained since that brick was last refilled, when the document has gained it by APPENDING to the layer an append extends and nothing else. A refill's own output is the accumulator the edit list reached at that brick's lattice — exact, in float32 — so it is what the next refill continues from, and a dab then costs what the dab adds rather than what the document holds.

The values SHALL be identical to a full refill's, BIT FOR BIT. Continuing a fold from the value it reached runs the same instructions in the same order over the same floats, so a tolerance would admit an error that is not there to admit.

COLOUR SHALL be carried the same way, and the seed SHALL carry it. What the accumulator IS decides what a seed must hold: a distance-only walk folds one float per sample and a coloured one folds a distance and a colour together, so continuing a coloured fold from a distance alone would fold every combine against black. A refill asked for colour SHALL therefore resume only from a seed that kept one, and SHALL fall back rather than invent it.

MORE THAN ONE VISIBLE SDF LAYER SHALL NOT prevent resuming, and the seed SHALL keep the two accumulators apart. The layers hard-union left to right, so where an append resumes from, the field is two values — the layers beneath the active one, and the active layer's own chain — and their union cannot be taken apart again. Seeding from the union is exact only where every appended item unions hard, which a blended dab does not. The half beneath SHALL be held as its own value, carried forward untouched while the active layer is sculpted, and folded in with the same hard union a whole-document compile emits between layers.

A part of a document compiled for that split SHALL cull under the WHOLE document's pad. A part compiled under its own smaller pad drops items the whole compile keeps, and the halves then no longer sum to the whole.

A refill SHALL fall back to evaluating in full wherever continuing would not be exact: the appended items would be culled differently from the value being continued; the cull pad has moved; the brick's own previous compile of the active layer produced no accumulator; colour is asked for and the seed kept none; or the edit was not an append to the layer an append extends.

Kept values SHALL be bounded IN BYTES rather than in bricks, since a brick may carry a colour and a half beneath as well as a distance, and SHALL be discarded on any edit that is not such an append.

#### Scenario: A stroke's refills equal a document built fresh
- **GIVEN** a document refilled once, then appended to and refilled again, dab after dab
- **WHEN** each refill is compared with one from a document holding the same items and no history to resume from
- **THEN** every sample is the same float

#### Scenario: A layer beneath is folded in rather than replayed
- **GIVEN** two visible SDF layers, the upper one being sculpted and the lower one overlapping the bricks read
- **WHEN** a stroke is refilled dab by dab
- **THEN** every sample equals a full refill's, and what the refill costs is set by the dab rather than by either layer's length

#### Scenario: An edit to the layer beneath is not resumed
- **WHEN** an item is added to a layer BENEATH the one being sculpted
- **THEN** the bricks are evaluated in full and the values equal a full refill's

#### Scenario: Colour survives the resumed path
- **WHEN** a refill asks for colour as well as distance, and the bricks carry seeds that kept colour
- **THEN** the distances and the colours both equal a full refill's, bit for bit

#### Scenario: An edit that is not an append is not resumed
- **WHEN** an item is removed or changed rather than appended, and the bricks are refilled
- **THEN** the values equal a full refill's
