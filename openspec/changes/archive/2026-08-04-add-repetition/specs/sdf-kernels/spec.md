# sdf-kernels — repetition reachable from a document

Delta for `add-repetition`.

## MODIFIED Requirements

### Requirement: Repetition operators
`repeat.h` SHALL provide infinite grid repetition (round-based), finite grid repetition with clamped cell index and neighbor-cell evaluation to avoid boundary artifacts (01 §2.4), and radial/circular arrays evaluated in O(2) sector evaluations, with per-element transform overrides.

All three SHALL be expressible in the tape as per-item modifiers, so a document can build arrays. The exactness condition SHALL be enforced rather than assumed: repetition preserves an exact field only when the item's extent plus its rounding and blend influence fits within its half-cell (or angular sector), and the compiler SHALL downgrade the tracked field info to a bound when it does not.

#### Scenario: Finite repetition boundary correctness
- **WHEN** the field of a finite N×N×N repetition is sampled near a cell boundary or outside the array extent
- **THEN** the distance accounts for neighboring cells (no seam discontinuities, no phantom copies beyond the extent)

#### Scenario: Repetition through the tape matches the kernel
- **WHEN** a document contains an item with a finite grid repetition
- **THEN** the compiled tape evaluates identically to applying `crep_lim_point` and the primitive directly

#### Scenario: Overflowing the half-cell downgrades exactness
- **WHEN** a repeated item's extent plus blend influence exceeds half its cell spacing
- **THEN** the tape reports a non-exact field, and stepping by its safe step scale still never crosses the surface
