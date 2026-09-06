# c-abi — a resumed refill carries colour

Delta for `resume-with-colour`.

## MODIFIED Requirements

### Requirement: A brick refill continues from its own previous result
Refilling a brick SHALL evaluate only what the document gained since that brick was last refilled, when the document has gained it by APPENDING and nothing else. A refill's own output is the accumulator the edit list reached at that brick's lattice — exact, in float32 — so it is what the next refill continues from, and a dab then costs what the dab adds rather than what the document holds.

The values SHALL be identical to a full refill's, BIT FOR BIT. Continuing a fold from the value it reached runs the same instructions in the same order over the same floats, so a tolerance would admit an error that is not there to admit.

COLOUR SHALL be carried the same way, and the seed SHALL carry it. What the accumulator IS decides what a seed must hold: a distance-only walk folds one float per sample and a coloured one folds a distance and a colour together, so continuing a coloured fold from a distance alone would fold every combine against black. A refill asked for colour SHALL therefore resume only from a seed that kept one, and SHALL fall back rather than invent it.

A refill SHALL fall back to evaluating in full wherever continuing would not be exact. Each of these is a case where it would not:

- the appended items would be culled differently from the value being continued, so the suffix SHALL be culled against the brick exactly as a whole-document compile culls;
- the cull pad has moved, since the pad decides which items a brick's compile keeps and a value taken under a different one continues a different field;
- the brick's own previous compile produced no accumulator, since a suffix continuing nothing would combine against far-outside rather than seed the chain;
- colour is asked for and the seed kept none;
- more than one SDF layer is visible, since a value may then sit beneath a union it does not describe.

Kept values SHALL be bounded IN BYTES rather than in bricks, since a coloured brick carries four times the floats of a distance-only one, and SHALL be discarded on any edit that is not an append.

#### Scenario: A stroke's refills equal a document built fresh
- **GIVEN** a document refilled once, then appended to and refilled again, dab after dab
- **WHEN** each refill is compared with one from a document holding the same items and no history to resume from
- **THEN** every sample is the same float

#### Scenario: Colour survives the resumed path
- **WHEN** a refill asks for colour as well as distance, and the bricks carry seeds that kept colour
- **THEN** the distances and the colours both equal a full refill's, bit for bit

#### Scenario: A colourless seed cannot serve a coloured refill
- **WHEN** a brick was last refilled without colour and the next refill asks for it
- **THEN** that brick is evaluated in full, and its colours are a full refill's

#### Scenario: The saving follows the dab
- **WHEN** the same dab is refilled into documents whose edit lists differ greatly in length
- **THEN** what the refill costs is set by the dab rather than by the length

#### Scenario: An edit that is not an append is not resumed
- **WHEN** an item is removed or changed rather than appended, and the bricks are refilled
- **THEN** the values equal a full refill's
