# c-abi — a drag under symmetry reaches every image and keeps its history

Delta for `reflect-the-brush-not-the-bound`.

## ADDED Requirements

### Requirement: A drag under symmetry reaches every image and keeps its history
`clay_layer_move_surface` SHALL state its reach as one box per image the layer's symmetry makes of the drag — the ball, one reflection per mirror axis, one rotation per radial copy — since the copies a mirrored or radial layer emits move where the images are. The boxes SHALL be taken as one invalidation, one revision, under one lock, and SHALL NOT be replaced by their union: the union of two balls a diameter apart is the slab between them, which under a mirror is the whole document.

Under symmetry the drag SHALL state the frontier of the items it actually reaches, so a mirrored drag on late-history items keeps the prefix seeds it would keep unmirrored. A drag whose image does reach root ordinal 0 still takes the legacy drop, by design.

`*out_applied` and the preview SHALL count ITEMS: an item that both the ball and an image reach takes its grabs in one command and is reported once.

A live Move transaction SHALL report every grab the last update resolved for an affected node — `clay_sdf_move_preview_grab_count` gives how many, one without symmetry and one per image of the drag that reaches the node with it, and `clay_sdf_move_preview_grab` gives the grab at an index, refusing an index past the count — since a host that drew the first grab alone would preview half of a straddler's drag.

#### Scenario: The reflected side is not stale
- **GIVEN** a mirrored layer and bricks warm on the side the ball's reflection covers
- **WHEN** a drag is applied on the other side and those bricks are refilled
- **THEN** the values match a fresh mirrored document's cold refill bit for bit, and differ from the undragged document's

#### Scenario: A mirrored drag resumes
- **GIVEN** a mirrored layer whose dragged items were appended last, and a warm window of bricks over them
- **WHEN** a continuing drag is applied frame after frame
- **THEN** the frontier probe reports the dragged items' own root ordinal, the window resumes exactly as the unmirrored layer's does, and the refill matches a fresh mirrored oracle bit for bit

#### Scenario: A transaction reports one grab per reaching image
- **WHEN** a live Move on an unmirrored layer is asked for an affected node's grabs
- **THEN** the count is one and index 1 is refused, while a node the drag does not reach is not found

#### Scenario: A straddling item counts once
- **WHEN** a mirrored drag's ball and reflection both reach one item
- **THEN** the preview names it once and `*out_applied` counts it once, and the two agree
