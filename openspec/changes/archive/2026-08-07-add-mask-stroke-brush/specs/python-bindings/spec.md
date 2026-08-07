# python-bindings — the mask brush

Delta for `add-mask-stroke-brush`.

## ADDED Requirements

### Requirement: Masking from Python is a stroke
The module SHALL let a script paint a mask from a resolved stroke, invert a mask within a box, and fill a box with a value. It SHALL accept a mask on the volume relax and flatten entry points.

The binding SHALL state that the footprint is derived from the stamp's world radius, so a script does not go looking for a size in mask cells and find none.

#### Scenario: A script paints a mask along a stroke
- **WHEN** a script resolves a stroke and applies it to a mask
- **THEN** the mask reads masked along the path

#### Scenario: A script freezes a region and relaxes around it
- **WHEN** a script relaxes a volume with a mask covering part of the relaxed region
- **THEN** the field under the mask is unchanged and the field beside it is smoothed
