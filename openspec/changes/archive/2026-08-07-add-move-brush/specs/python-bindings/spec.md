# python-bindings — the Move brush

Delta for `add-move-brush`.

## ADDED Requirements

### Requirement: The Move brush from Python
The module SHALL let a script drag a layer's surface with a world centre, radius and displacement, and SHALL report which nodes took a warp.

The binding SHALL state that this differs from putting a `grab` on one item — that a grab is per item and in that item's own frame, so it pulls one item's share of a blended form and leaves the rest — because a script author reaching for `prim.grab(...)` will otherwise not find out until the result looks wrong.

It SHALL also state that the surface moves less than the displacement asked for, and why.

#### Scenario: A script drags a blended form
- **WHEN** a script moves a layer built from two blended items, centred between them
- **THEN** both sides move and the result is symmetric about the drag's centre

#### Scenario: The whole drag is one undo step
- **WHEN** a script with undo enabled moves a form and undoes it
- **THEN** the document is back where it started in a single step
