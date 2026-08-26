# c-abi — a brick refill resumes from what the last one produced

Delta for `resume-the-brick-refill`.

## ADDED Requirements

### Requirement: A brick refill continues from its own previous result
Refilling a brick SHALL evaluate only what the document gained since that brick was last refilled, when the document has gained it by APPENDING and nothing else. A refill's own output is the accumulator the edit list reached at that brick's lattice — exact, in float32 — so it is what the next refill continues from, and a dab then costs what the dab adds rather than what the document holds.

The values SHALL be identical to a full refill's, BIT FOR BIT. Continuing a fold from the value it reached runs the same instructions in the same order over the same floats, so a tolerance would admit an error that is not there to admit.

A refill SHALL fall back to evaluating in full wherever continuing would not be exact. Each of these is a case where it would not:

- the appended items would be culled differently from the value being continued, so the suffix SHALL be culled against the brick exactly as a whole-document compile culls;
- the cull pad has moved, since the pad decides which items a brick's compile keeps and a value taken under a different one continues a different field;
- the brick's own previous compile produced no accumulator, since a suffix continuing nothing would combine against far-outside rather than seed the chain;
- colour was asked for, since a seed is one distance per sample;
- more than one SDF layer is visible, since a value may then sit beneath a union it does not describe.

Kept values SHALL be bounded and SHALL be discarded on any edit that is not an append, because no seed can be carried across one.

#### Scenario: A stroke's refills equal a document built fresh
- **GIVEN** a document refilled once, then appended to and refilled again, dab after dab
- **WHEN** each refill is compared with one from a document holding the same items and no history to resume from
- **THEN** every sample is the same float

#### Scenario: The saving follows the dab
- **WHEN** the same dab is refilled into documents whose edit lists differ greatly in length
- **THEN** what the refill costs is set by the dab rather than by the length

#### Scenario: An edit that is not an append is not resumed
- **WHEN** an item is removed or changed rather than appended, and the bricks are refilled
- **THEN** the values equal a full refill's

#### Scenario: Colour takes the full path
- **WHEN** a refill asks for colour as well as distance
- **THEN** the values and colours equal a full refill's
