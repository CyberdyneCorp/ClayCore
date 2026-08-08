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

### Requirement: A world drag resolves into a field-level move
The library SHALL resolve a world-space centre, radius and displacement into the per-item warps that reproduce that drag on the ASSEMBLED surface of a layer, rather than on one item of it. The result SHALL be returned rather than applied, so a host can preview a drag before committing it and decide which commands carry it — the same rule the stroke engine's node consumer follows.

Each warp SHALL be expressed in its item's OWN frame, mapping the world centre, radius and displacement through that item's world transform — which is the layer's transform composed with the item's, because a group's transform does not reach its children in this scene model. The resolver SHALL agree with the evaluator on that point rather than accumulating a chain the evaluator does not.

Each warp SHALL be marked for the FRONT of its item's chain, because a warp appended behind an existing deformer has its region weight evaluated at a point that deformer already moved.

An item whose influence bounds do not reach the drag's region SHALL receive no warp at all: a deformer with finite support, outside its own support, is a no-op that still costs a tape record on every evaluation.

#### Scenario: A blended form moves as one surface
- **WHEN** a drag centred between two smooth-unioned items is resolved and applied
- **THEN** both sides lift, symmetrically, and the lift peaks at the world centre

#### Scenario: Grabbing one item is not the same thing
- **WHEN** the same drag is expressed as a grab on a single item instead
- **THEN** that item's side moves and the other is left behind

#### Scenario: A nested item moves where the drag was aimed
- **WHEN** the layer's items sit under a group and a drag is resolved
- **THEN** the surface moves where the drag was aimed in WORLD space, matching what the evaluator does with those items

#### Scenario: A transformed layer maps correctly
- **WHEN** the layer carries a transform and a drag is resolved in world space
- **THEN** the surface moves where the drag was aimed, not where it would have landed in layer space

#### Scenario: Items out of reach are skipped
- **WHEN** a layer holds items far outside the drag's radius
- **THEN** no warp is produced for them

#### Scenario: Nothing is written
- **WHEN** a drag is resolved
- **THEN** the document is unchanged until the caller applies the result

### Requirement: The Move brush inherits grab's pull, and says so
The move SHALL use the existing `grab` deformation rather than a new one, and SHALL therefore inherit its behaviour: the surface moves LESS than the displacement asked for, because the region weight is taken at the sample point rather than at its preimage.

This SHALL be documented rather than corrected. Solving for the true preimage costs an iteration per sample and buys nothing a sculptor can feel, and the pull is monotonic in the displacement, so a UI can calibrate against it.

#### Scenario: The pull is monotonic
- **WHEN** the same drag is resolved at increasing displacements
- **THEN** the surface moves further each time, by less than the displacement given

### Requirement: A drag coalesces rather than accumulating
A Move applied repeatedly as one gesture SHALL replace that gesture's warp rather than adding another beside it. A drag holds its centre and radius fixed and grows only its displacement, so those two identify the gesture without a caller having to thread an identifier through.

Otherwise a drag adds one warp per frame: the chain grows without bound, and because each warp multiplies into the declared Lipschitz, the safe step scale collapses over the length of the gesture.

A drag whose centre or radius differs is a different gesture and SHALL be kept beside the first. A leading deformer that is not a grab from a drag SHALL NOT be replaced.

#### Scenario: Frames of one drag do not stack
- **WHEN** a Move is applied repeatedly with a growing displacement and a fixed centre and radius
- **THEN** each item carries exactly one warp from that drag, and the field equals a single drag of the final displacement

#### Scenario: The marcher does not pay for the frame count
- **WHEN** the same drag is applied in many frames rather than one
- **THEN** the resulting safe step scale is the same

#### Scenario: A different gesture is kept
- **WHEN** a Move with a different centre follows one already applied
- **THEN** the earlier warp is kept and the new one is added in front of it

#### Scenario: An unrelated chain is left alone
- **WHEN** a Move is applied to an item whose chain already begins with a deformer that is not this drag
- **THEN** that deformer is kept, and the move goes in front of it

### Requirement: A move can be previewed without applying it
The library SHALL expose which items a drag would warp without modifying the document, so a host can preview a Move, or show what it is about to affect, before committing it.

#### Scenario: Previewing changes nothing
- **WHEN** a drag is previewed
- **THEN** the document evaluates identically to before, and the preview names the items the move would warp

#### Scenario: The preview agrees with the move
- **WHEN** a drag is previewed and then applied
- **THEN** the items reported by each are the same

### Requirement: A stamp's strength scales an amplitude, where the op has one
Turning stamps into edit-list items SHALL apply a stamp's strength to the item's amplitude for the ops whose `blend.k` IS an amplitude — relief and incise — so that the stroke engine's pressure-strength channel and its accumulation mode reach an SDF layer.

For every other op `blend.k` is a radius, a depth or a half-thickness. Scaling those by a stroke's strength would change the SHAPE rather than the amount, differently per op and silently, so those ops SHALL continue to ignore strength. A boolean has no partial application: a union at half strength is not a smaller union.

#### Scenario: Buildup accumulates past clamped
- **WHEN** the same dense relief stroke is applied under buildup and under clamped accumulation
- **THEN** the buildup pass moves the surface further

#### Scenario: A lighter touch deposits less
- **WHEN** a relief stroke is applied with reduced pressure strength
- **THEN** the surface moves less than at full strength

#### Scenario: A boolean stroke is unaffected
- **WHEN** an add stroke is applied under buildup and under clamped accumulation
- **THEN** the two results are identical

### Requirement: A drawn path resolves into a tube
The library SHALL resolve a path of control points and a handful of settings into an ordinary edit item describing a tube along that path — a rope, pipe, tentacle or hair strand. Resolving SHALL be pure: no document is read or touched, so a caller can preview a tube before committing it.

The result SHALL be an ordinary item, so undo, serialization, picking, masking and meshing apply to it unchanged.

#### Scenario: A path becomes a tube
- **WHEN** a path of control points is resolved
- **THEN** the result is an item whose surface follows that path at the requested radius

#### Scenario: It is an ordinary item
- **WHEN** a resolved tube is added to a layer
- **THEN** it combines, saves, picks and meshes exactly as any other item does

### Requirement: The radius may vary along the tube
The radius SHALL be settable at the start, the middle and the end, and interpolated between them by ARC LENGTH so that a path whose control points bunch does not bunch the taper.

#### Scenario: A tapered tube
- **WHEN** a tube is resolved with different radii at its start and end
- **THEN** its thickness changes along its length, reaching each radius where that radius was asked for

#### Scenario: A uniform tube
- **WHEN** all three radii are equal
- **THEN** the thickness is constant along the tube

#### Scenario: The taper follows arc length, not point index
- **WHEN** the same path is resolved with its control points evenly spaced, and again with them bunched at one end
- **THEN** the radius at a given fraction of the length is the same in both

### Requirement: The cross-section decides the representation
A tube with no profile SHALL be a swept sphere, which is an EXACT distance field. A tube with a profile SHALL be a swept item, which is a bound field and costs safe step scale.

This is chosen by whether a profile is given rather than exposed as a separate flag, since a caller asking for a square cross-section has already said which one it wants.

#### Scenario: A round tube stays exact
- **WHEN** a tube with no profile is compiled
- **THEN** the document reports the field as exact and the safe step scale is 1

#### Scenario: A profiled tube declares its cost
- **WHEN** a tube with a profile is compiled
- **THEN** the field reports as inexact and the safe step scale is below 1

### Requirement: Smoothness is the point type
Whether the tube runs smoothly through its control points or turns sharply at them SHALL be the curve's existing point type rather than a separate toggle, so a tube's path is the same kind of curve every other item takes.

#### Scenario: Sharp against smooth
- **WHEN** the same points are resolved with hard points and with B-spline points
- **THEN** the two fields differ, and the B-spline one passes further from the corner

#### Scenario: A closed tube joins
- **WHEN** a tube is resolved closed
- **THEN** its last point joins back to its first, with no cap between them

### Requirement: A path that is not one is refused
Fewer points than describe a path, or a radius that is not positive anywhere, SHALL be refused rather than yielding an item that contributes nothing.

#### Scenario: Degenerate input
- **WHEN** a tube is asked for from a single point, or with every radius zero
- **THEN** it is refused

