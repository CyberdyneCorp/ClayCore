# brush-engine — the mask brush

Delta for `add-mask-stroke-brush`.

## ADDED Requirements

### Requirement: A stroke can paint a mask
The stroke engine SHALL provide a consumer that paints a mask from resolved stamps, alongside the consumers that write voxels and emit edit-list nodes. Masking SHALL therefore be the same gesture as sculpting, resolved by the same code: spacing, pressure, taper, steady stroke and jitter apply to a mask stroke exactly as they apply to any other.

The consumer SHALL convert each stamp's WORLD radius into a footprint sized in MASK cells, so that a stroke covers the same world region whatever the mask's cell size is. A caller SHALL NOT have to make that conversion, because a caller that makes it differently gets a mask stroke whose width changes with the mask's resolution.

The consumer SHALL take a target value rather than a direction, so that painting and erasing are the same call — target 1 masks, target 0 releases.

It SHALL NOT take a mask to gate itself against: a mask does not gate its own painting.

#### Scenario: A drag paints a band of mask
- **WHEN** a stroke is resolved and applied to a mask
- **THEN** the mask reads masked along the path and unmasked well away from it

#### Scenario: The mask's resolution does not change the stroke's width
- **WHEN** the same stroke is applied to two masks whose cell sizes differ
- **THEN** both cover the same world region, to within a cell

#### Scenario: Erasing is the same call
- **WHEN** a stroke is applied over a painted region with target 0
- **THEN** the region it covers reads unmasked afterwards

### Requirement: Accumulation over a mask
A mask cell moves TOWARD the target by the brush weight rather than accumulating a quantity, so the two accumulation modes SHALL be defined against that. Under `Buildup` overlapping stamps SHALL approach the target and SHALL NOT overshoot it. Under `Clamped` a stroke SHALL reach the target once however many stamps overlap, by the same per-stamp division the other consumers use.

Values SHALL remain within [0,1] under any number of overlapping stamps.

#### Scenario: Buildup approaches the target
- **WHEN** a slow stroke with many overlapping stamps is painted at partial strength
- **THEN** the covered cells approach the target without exceeding it

#### Scenario: Clamped reaches it once
- **WHEN** the same stroke is painted with clamped accumulation
- **THEN** the covered cells reach roughly the target rather than saturating past what one pass would give
