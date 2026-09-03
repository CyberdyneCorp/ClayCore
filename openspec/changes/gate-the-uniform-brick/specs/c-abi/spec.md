# c-abi — a brick proven uniform is not walked

Delta for `gate-the-uniform-brick`.

## ADDED Requirements

### Requirement: a brick proven uniform is classified without a walk

`clay_brick_cache_eval_requests` and `clay_brick_cache_eval_requests_device`
MAY answer a brick without evaluating its lattice when one evaluation of the
brick's own culled tape at the lattice centre, together with that tape's
declared Lipschitz bound, proves every sample beyond the band with the centre's
sign. The ball SHALL be the lattice's own — its centre and its half-diagonal —
and the bound SHALL be the brick's own culled tape's, never the whole
document's. Only the whole-document evaluation may be so gated; the per-layer
halves a multi-layer refill evaluates SHALL always be walked.

What `clay_brick_cache_submit` stores for a gated brick — its state, and its
uniform colour, read from sample dim^3/2 — SHALL be bit-identical to what it
would have stored from the walked samples. The values written to a gated
brick's slot SHALL every one lie beyond the band with the brick's sign and
carry the field's own colour at sample dim^3/2; they are otherwise a stand-in,
and the entry point documents them as such.

A gated brick SHALL NOT store its stand-in values as a seed. It SHALL store the
proof in the seed's place, and a later refill SHALL either carry that proof
through the appended items — folding them onto the stored centre and colour
sample values with the walk's own arithmetic, and re-proving under a bound that
is exact for what was appended — or take the full path. A refill that resumes
from a proof SHALL produce, after submit, the same stored brick as a refill of
the same document from scratch.

A proof SHALL count as refilled where it is made and as resumed where a later
refill carries it, so the ratio `clay_resume_stats` documents keeps its
meaning.

#### Scenario: a fill with and without the gate stores the same bricks
- **GIVEN** two documents holding the same worked, coloured sculpt
- **WHEN** every brick of the model is refilled and submitted for each, one with the gate disabled
- **THEN** every brick's state, stored halves and stored colours are identical between the two caches
- **AND** the gated document proved at least half of the uniform bricks and no surface brick

#### Scenario: a dab after a proof
- **GIVEN** a window filled with the gate, some of whose bricks were proven uniform
- **WHEN** an item that reaches those bricks but leaves them uniform is appended and the window is refilled
- **THEN** every brick of the window resumes, none walks
- **AND** the submitted cache equals one filled from scratch on a document holding the same items, with the gate enabled or disabled

#### Scenario: a carve that reaches a proven brick
- **GIVEN** the same window, warm
- **WHEN** a subtracted item brings the surface into bricks that were proven uniform
- **THEN** those bricks take the full path
- **AND** the submitted cache equals one filled from scratch

#### Scenario: a multi-layer refill is never gated
- **GIVEN** a document with two visible SDF layers
- **WHEN** a window is refilled
- **THEN** no brick is proven, and the cache equals one filled with the gate disabled

#### Scenario: a device backend classifies a gated brick as the cpu does
- **GIVEN** the same worked sculpt refilled through the cpu backend and through a device backend
- **WHEN** the two caches are compared over the bricks the gate proved
- **THEN** every such brick has the same state and the same uniform colour in both
