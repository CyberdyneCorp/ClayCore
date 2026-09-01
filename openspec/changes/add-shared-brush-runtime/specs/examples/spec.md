# examples — one gesture, three representations

Delta for `add-shared-brush-runtime`.

## ADDED Requirements

### Requirement: A numbered example replays one preset gesture across every representation
The gallery SHALL carry an example that resolves ONE gesture from the reference preset library and replays it over a fixed mesh, an adaptive surface and a multiresolution hierarchy built from the same source model, renders all three, and asserts its claims rather than describing them.

It SHALL assert BOTH halves. What must match: a normal-free verb writes byte-identical positions on all three, a topological automask reaches the same vertex set, and the scratch arena stops growing over the stroke. What legitimately differs: a verb that reads a vertex normal diverges between the fixed and adaptive surfaces because their normal estimators differ, and the example SHALL say why in prose and bound the divergence rather than hide it.

It SHALL raise `SystemExit` when a claim stops holding, which is the convention every other example in the gallery follows.

#### Scenario: The example fails when a representation drifts
- **WHEN** a change makes one representation's normal-free stamp disagree with another's
- **THEN** the example exits non-zero and names the claim that stopped holding

#### Scenario: The example fails when the divergence vanishes
- **WHEN** a change silently unifies the two normal estimators
- **THEN** the example exits non-zero, because a difference it asserts has disappeared
