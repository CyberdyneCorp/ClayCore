# python-bindings — scale an item per axis

Delta for `scale-an-item-per-axis`.

## ADDED Requirements

### Requirement: scale= takes one number or three
Every place the Python bindings accept a placement scale SHALL accept either one number, meaning a uniform scale as it always did, or a sequence of three, meaning a per-axis one. Python can carry both in a single argument where C needs a second entry point, so the bindings SHALL NOT grow a parallel set of calls.

A component that is not greater than zero, and a sequence that is not of length three, SHALL be refused.

Because these bindings take PARTIAL updates — unlike the C ABI, whose setters take the whole transform — a call that says nothing about scale SHALL leave BOTH halves of an item's scale where they were. Moving a squashed item must not un-squash it.

#### Scenario: One number still means what it meant
- **WHEN** an item is placed with a scalar scale
- **THEN** it evaluates identically to the same item placed with three equal numbers, and both keep the field exact

#### Scenario: Three numbers squash the item
- **WHEN** a unit sphere is placed with scale=(2, 1, 1)
- **THEN** its bounds and its field report a surface crossing x at 2 and y at 1

#### Scenario: A partial update leaves a squash alone
- **WHEN** a squashed node is retransformed with a position and no scale
- **THEN** it moves and stays squashed
