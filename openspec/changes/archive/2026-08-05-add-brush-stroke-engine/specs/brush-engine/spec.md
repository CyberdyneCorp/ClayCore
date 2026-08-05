# brush-engine

Delta for `add-brush-stroke-engine`.

## ADDED Requirements

### Requirement: Strokes resolve to stamps
The module SHALL resolve a sequence of stroke samples — position, pressure, tilt and a monotone path parameter — into an ordered list of stamps, each carrying a position, radius, strength and orientation. Resolution SHALL be pure: it SHALL NOT read or modify a document.

Stamps SHALL be spaced along the path at a preset-controlled fraction of the brush diameter, so a fast drag and a slow one over the same path produce the same stamps.

#### Scenario: Spacing is by distance, not by sample
- **WHEN** the same path is drawn with sparse samples and with dense ones
- **THEN** the resolved stamps are the same

#### Scenario: A path shorter than one spacing still stamps
- **WHEN** a stroke has a single sample, or a path shorter than the spacing
- **THEN** exactly one stamp is produced, at the stroke's start

### Requirement: Presets shape the stroke
A preset SHALL control spacing, position and size jitter, pressure-to-size and pressure-to-strength response, rotate-along-stroke, taper at the ends, steady-stroke smoothing of the input path, and whether overlapping stamps accumulate or clamp.

Jitter SHALL be derived from the stamp index and a seed rather than from a random source, so a stroke resolves identically on every platform and through every binding.

#### Scenario: Pressure drives size
- **WHEN** a stroke ramps pressure from low to high under a preset mapping pressure to size
- **THEN** stamp radius rises monotonically along the stroke

#### Scenario: Taper closes the ends
- **WHEN** a preset tapers both ends
- **THEN** the first and last stamps are smaller than those in the middle

#### Scenario: Jitter is reproducible
- **WHEN** the same stroke is resolved twice with the same preset and seed
- **THEN** the stamps are identical, and a different seed gives different ones

#### Scenario: Steady stroke smooths the path
- **WHEN** a jagged path is resolved with steady-stroke enabled
- **THEN** the stamp positions deviate less from the path's overall direction than the samples do

### Requirement: Presets survive engine versions
A preset SHALL carry a schema version and SHALL serialize and deserialize deterministically. Loading a preset written by an older version SHALL succeed, taking defaults for anything it did not carry. Loading one from a newer version SHALL be refused with a typed error rather than partially applied.

#### Scenario: An older preset still loads
- **WHEN** a preset written at an earlier schema version is loaded
- **THEN** it loads, and the fields it did not carry take their defaults

#### Scenario: A newer preset is refused, not guessed at
- **WHEN** a preset declaring a newer schema version is loaded
- **THEN** it is rejected with a typed error and nothing is partially applied

### Requirement: Stamps become ordinary edits
The module SHALL apply a stamp list to a voxel grid as brush applications, and to an SDF layer as nodes appended through the existing command vocabulary. It SHALL NOT introduce a separate evaluation path for stroked edits, so undo, coalescing, serialization and picking apply to them unchanged.

#### Scenario: A stroked SDF edit is an ordinary edit list
- **WHEN** a stroke is applied to an SDF layer
- **THEN** the layer's edit list gains one node per stamp, and undoing the stroke restores the document exactly

#### Scenario: A stroked voxel edit round trips
- **WHEN** a stroke is applied to a voxel layer and the document is saved and reloaded
- **THEN** the grid is unchanged

### Requirement: Strokes consume a mask
Stroke application SHALL accept an optional mask. A stamp centred in a fully masked region SHALL be dropped, and one in a partially masked region SHALL have its strength scaled by one minus the mask there. This is how a declarative SDF edit is frozen: the region receives no new items.

#### Scenario: A frozen region receives no items
- **WHEN** a stroke crosses a fully masked region on an SDF layer
- **THEN** no node is appended for the stamps inside it, and nodes outside it are appended as usual
