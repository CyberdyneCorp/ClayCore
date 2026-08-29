# scene-model — a bake that does not need a layer

Delta for `add-sdf-prefix-cache`.

## ADDED Requirements

### Requirement: An already-compiled tape can be baked into a volume
Sampling a layer's field into a volume is two things: compiling the layer into a tape in the layer's own frame, and sampling that tape. The second SHALL be reachable on its own, for a caller that holds a tape belonging to no layer — a PREFIX of a layer's edit list is exactly such a tape.

It SHALL be the same code the layer form runs: the same sampling, the same post-process, the same colour pass, the same measured steepness. A volume produced this way SHALL be one a consolidation could have produced, so that there is ONE definition of what a baked volume is rather than two that agree today.

The caller SHALL owe the two things a tape cannot state for itself, and the interface SHALL make both explicit rather than guess:

- THE FRAME. A layer bake compiles a local view — the layer visible and its own transform identity — because sampling the world-space field and then putting the result back under the layer applies the transform twice. A caller compiling its own tape owes the same convention.
- WHETHER COLOUR IS CARRIED. The compiler folds colour into instructions, so by the time a tape exists the question "can this produce more than one colour" can no longer be asked of it. The rule that answers it for a layer SHALL stay reachable, because it is the rule any caller has to apply to get the same bytes — and passing the wrong answer produces different BYTES, not a slower path.

The tape SHALL be borrowed rather than owned, and SHALL outlive the call. Refusals SHALL be the layer form's refusals — no resolution, nothing to sample — and a cancelled bake SHALL discard rather than return a partial volume.

#### Scenario: Baking a tape reproduces baking its layer
- **GIVEN** a layer, and the tape compiled from it under the frame convention above
- **WHEN** each is baked with the same parameters and the same colour answer
- **THEN** the two volumes serialize to the same bytes

#### Scenario: The colour answer is the caller's, and it changes the bytes
- **WHEN** a tape from a one-colour layer and a tape from a many-colour layer are each baked with the colour answer their layers give
- **THEN** each volume carries a colour channel exactly when its layer's bake does, and each matches its layer's bake byte for byte

#### Scenario: A tape bake refuses what a layer bake refuses
- **WHEN** a bake is attempted with no resolution, or on a tape with nothing to sample
- **THEN** it produces nothing
- **AND WHEN** a bake is cancelled through its token
- **THEN** it produces nothing rather than a partial volume
