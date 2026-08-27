# c-abi

## ADDED Requirements

### Requirement: A gesture invalidates once, for the region it states

An entry point that applies many commands as one gesture MAY state the region
those commands can reach and invalidate once for all of them, instead of having
a region derived per command.

A stated region SHALL cover everything the gesture changes. An entry point that
cannot state its reach SHALL keep the derived per-command region, which is
always correct.

The invalidation SHALL happen even when the gesture fails part way, because a
gesture that applied some of its commands has still changed the document.

`clay_layer_move_surface` SHALL state the drag's own region: the ball of the
drag radius about its centre, dilated by the displacement. Outside that ball the
warp's weight is zero, so no sample can evaluate differently.

#### Scenario: A drag across bricks the cache holds
- **GIVEN** a document whose bricks have been filled once
- **WHEN** a drag moves the surface those bricks read
- **THEN** a refill returns exactly what a full evaluation of the dragged document returns

#### Scenario: A drag repeated
- **GIVEN** a drag whose refill has seeded the bricks again
- **WHEN** the same drag is applied a second time
- **THEN** a refill still returns what a full evaluation returns

#### Scenario: A drag the bricks cannot reach
- **WHEN** a drag moves an item no brick under consideration reads
- **THEN** those bricks return what they returned before

#### Scenario: A gesture that fails part way
- **WHEN** a gesture applies some commands and then refuses
- **THEN** the region it stated is still invalidated
