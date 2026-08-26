# c-abi — an edit invalidates what it reaches

Delta for `invalidate-by-region`.

## ADDED Requirements

### Requirement: An edit only invalidates the bricks it can reach
An edit that is not an append SHALL NOT discard a brick's kept value when the edit cannot change what that brick evaluates to. A kept value is the value of that brick's CULLED tape, and an item whose influence misses the brick's cull region is dropped from that tape — so editing it leaves the brick's answer exactly as it was, and the value SHALL be carried forward to the new revision rather than recomputed.

The region an edit reaches SHALL be taken on BOTH sides of it and unioned. One side is not an answer: an item being added is not there beforehand, one being removed is not there afterwards, and one being moved has two ends.

An edit whose region is EMPTY — one that cannot change what the document evaluates to, such as a rename — SHALL keep every value. One whose region is unbounded SHALL discard them all.

An edit whose reach is NOT known SHALL discard everything, and that SHALL remain the default. An entry point that does not positively know what it changed must land there, exactly as it must for an append.

A kept value already at the current revision SHALL be served as it is, since nothing remains to fold into it.

#### Scenario: An edit outside every brick read costs nothing
- **GIVEN** bricks refilled once, and an item edited that lies outside all of their cull regions
- **WHEN** those bricks are refilled again
- **THEN** the values equal a full refill's, and what the refill costs is set by the edit rather than by the length of the edit list

#### Scenario: An edit the bricks do reach is recomputed
- **WHEN** an item within the bricks' cull regions is removed
- **THEN** those bricks are evaluated again and their values equal a full refill's

#### Scenario: Refilling twice with no edit between costs nothing
- **WHEN** the same bricks are refilled twice and the document did not change
- **THEN** the second refill returns the values the first produced
