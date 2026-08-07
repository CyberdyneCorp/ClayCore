# brush-engine Specification

## Purpose
TBD - created by archiving change add-brush-stroke-engine. Update Purpose after archive.
## Requirements
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

### Requirement: A drag resolves into a tendril
The library SHALL resolve a surface anchor, an inward normal and a drag path into an ordinary edit item that reads as a tendril pulled from the surface. The result SHALL be a stroke item like any other, so undo, coalescing, serialization, picking and masking apply to it unchanged.

Resolving SHALL be pure: no document is read or touched, so a caller can preview a tendril before committing it.

#### Scenario: A drag becomes a tendril
- **WHEN** a drag path leading away from a surface point is resolved
- **THEN** the result is a stroke item whose field describes a tapering tendril along that path

#### Scenario: The result is an ordinary item
- **WHEN** a resolved tendril is added to a layer
- **THEN** it combines, saves and evaluates exactly as any other stroke does

#### Scenario: The field stays exact
- **WHEN** a document containing a resolved tendril is compiled
- **THEN** the safe step scale is unchanged from the document without it

### Requirement: A tendril begins where the user touched
The anchor SHALL be prepended to the drag path, so the tendril begins at the picked surface point rather than at the first drag sample. Those are not the same place: a pick reports the surface, and the first sample arrives a frame later with the finger already moving.

The anchor SHALL NOT be pushed inward. That was specified, as a fraction of the base radius, on the theory that a tendril anchored on the surface would leave a neck where the two fields meet. It does not — the sweep from a surface point already overlaps the body by its own radius, so a deeper anchor only adds material where the body is solid anyway, and the field around the base is identical at every depth. The parameter was removed rather than kept as a knob that does nothing.

#### Scenario: The tendril starts at the anchor, not at the first sample
- **WHEN** a tendril is resolved from a drag whose first sample is away from the picked point
- **THEN** the resolved item's first point is the picked point

#### Scenario: A tendril does not detach
- **WHEN** the field is sampled along the tendril from the body to the tip
- **THEN** it is solid the whole way, with no break

### Requirement: The taper follows arc length, not sample count
A hand moves at an uneven speed, so a drag's samples are unevenly spaced. The radius SHALL taper along the path's ARC LENGTH, so that how fast the gesture was does not change the tendril it produces.

The tip SHALL keep a stated minimum radius rather than tapering to nothing, so a tendril ends in a point that exists rather than in a feature too small for the sampling to carry.

#### Scenario: Gesture speed does not shape the tendril
- **WHEN** the same path is resolved from evenly spaced samples and from samples that bunch
- **THEN** the two tendrils have the same shape

#### Scenario: The tip is a point, not a vanishing
- **WHEN** a tendril is resolved with a taper that would reach zero
- **THEN** the tip radius stops at the stated minimum

#### Scenario: A tap still leaves a mark
- **WHEN** a drag shorter than one step is resolved
- **THEN** a small tendril is produced rather than nothing, the same rule the stroke engine follows

### Requirement: Degenerate input is refused where the item is built
A drag with no points, or one whose path has no length, SHALL be refused rather than resolved into an item that would contribute nothing or sit at the origin.

#### Scenario: An empty drag is refused
- **WHEN** a tendril is resolved from an empty path
- **THEN** the call fails rather than returning an item

#### Scenario: A degenerate normal is refused
- **WHEN** a tendril is resolved with a zero-length normal
- **THEN** the call fails rather than anchoring in an arbitrary direction

### Requirement: A world-space drag grabs the surface, not one item
The library SHALL resolve a world-space drag — a centre, a radius and a displacement — into a plan of per-item deformers that together drag the accumulated surface, rather than requiring the caller to place a deformer on one item.

The `grab` deformer acts on a single item's own field and takes its centre in that item's LOCAL frame. Those two facts make the obvious use wrong on any form built from more than one item: grabbing one item pulls its share and leaves the rest behind.

Resolving SHALL read the document but never modify it, so a caller can preview a Move before committing it.

#### Scenario: A multi-item form moves as one surface
- **WHEN** a world drag over a form smooth-unioned from several items is resolved and applied
- **THEN** every part of the surface within the drag's radius moves together, as it would if the form were a single item

#### Scenario: It agrees with a single-item grab
- **WHEN** the form IS a single item, and the same world drag is applied both through the resolver and as a hand-placed local grab
- **THEN** the two fields agree

#### Scenario: Resolving touches nothing
- **WHEN** a drag is resolved
- **THEN** the document is unchanged, and applying the returned plan is what changes it

### Requirement: The world-to-local mapping is exact
Mapping a world drag into an item's frame SHALL be exact rather than approximate. An item's world transform is its layer's transform composed with its own, and that transform's scale is uniform, so a spherical falloff stays spherical: the centre maps through the inverse transform, the displacement through the inverse rotation and scale, and the radius through the scale.

Applying one world warp to every operand is EQUAL to warping their combination, because combine ops are pointwise in the deformed point. The resolver therefore produces a true field-level grab, not an imitation of one.

#### Scenario: A transformed item is grabbed correctly
- **WHEN** a world drag is applied to a form whose items carry rotations, translations and uniform scales
- **THEN** the surface moves as the world drag describes, independently of how the items are individually placed

#### Scenario: The drag's centre is a world quantity
- **WHEN** the same world drag is resolved against two documents whose items are placed differently but whose surfaces coincide
- **THEN** the resulting surfaces coincide

### Requirement: A drag reaches only what it can touch
Only items whose influence bound intersects the drag's sphere SHALL receive a deformer. A grab breaks exactness and raises the Lipschitz bound, so placing one on every item would cost a whole document its safe step scale for a local gesture; and outside its radius the warp is the identity, so those items are provably unaffected.

#### Scenario: A distant item is left alone
- **WHEN** a drag is resolved against a document containing an item well outside its radius
- **THEN** that item receives no deformer, and the field around it is unchanged

#### Scenario: A local gesture stays cheap
- **WHEN** a drag reaching one item of many is applied
- **THEN** the document's safe step scale falls by no more than the same drag applied to that item alone

### Requirement: A drag coalesces rather than accumulating
During one drag the centre and radius are fixed and only the displacement grows. The resolver SHALL return the whole new deformer chain for each item, replacing a trailing grab that carries the same centre and radius rather than appending beside it.

Otherwise a drag would append one deformer per frame, growing the chain without bound and degrading both evaluation cost and the declared Lipschitz with every frame.

#### Scenario: A drag does not grow the chain
- **WHEN** a drag is resolved and applied repeatedly with a growing displacement
- **THEN** each item carries exactly one grab from that drag, and the surface follows the latest displacement

#### Scenario: A different drag is a different deformer
- **WHEN** a second drag with a different centre is resolved after the first is applied
- **THEN** the first drag's deformer is kept and the second is added beside it

### Requirement: A mirrored item is grabbed symmetrically
An item carrying its layer's mirror evaluates the same local deformer chain for every mirror copy, so a drag applied to it SHALL move the mirrored copies symmetrically. This is inherited from the local nature of the deformer rather than special-cased, and it is the behaviour symmetric sculpting wants.

#### Scenario: Symmetry follows the drag
- **WHEN** a drag is applied to a form whose items carry an active layer mirror
- **THEN** the mirrored side moves by the mirror image of the drag

