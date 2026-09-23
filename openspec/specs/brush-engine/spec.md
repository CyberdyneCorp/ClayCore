# brush-engine Specification

## Purpose
What a STROKE is, independently of what it lands on.

Spacing along the path, pressure response, deterministic jitter, taper, steady
stroke, buildup versus clamped accumulation, and versioned presets that survive
an engine version — resolved once into spaced stamps that each consumer then
applies in its own vocabulary.

Apart from the representations it drives, and that separation is the point: a
gesture must mean the same thing on a voxel layer, an SDF layer, a mask and a
mesh, and it can only do that if "what the gesture was" is decided in one place.

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

Under the layer's symmetry the drag SHALL be stated as its IMAGES — the ball itself, one reflection per set mirror axis and one rotation per radial copy, additively and never as products, which is exactly the set of copies the compiler emits of an item — and each item SHALL be tested on its OWN influence bound, without the reflected or rotated copies, against each image. An image that reaches an item yields one grab at that image's centre with that image's displacement, so the reflected ball grabs the items whose reflections sit under the ball; an item no image reaches SHALL receive no warp at all, since a deformer with finite support, outside its own support, is a no-op that still costs a tape record on every evaluation. An item that does not participate in the symmetry SHALL see the drag alone. A node's grabs SHALL be ordered by their values, never by which image produced them, and a warp SHALL name every image the item can see so that a continuing gesture can recognise all of its earlier frames.

Where the drag is resolved in two halves, the half that does not depend on the displacement SHALL carry the images the item sees — where each lands in the item's frame, and whether it reaches the item's own bound — so that the per-frame half maps the displacement through each image without the layer, and preparing once then resolving under symmetry is bit-identical to resolving in one step.

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

#### Scenario: Under a mirror the drag selects what the ball or its reflection touches
- **GIVEN** an x-mirrored layer holding a base ball on the plane, an item under the ball and an item whose reflection sits under the ball
- **WHEN** the drag is resolved
- **THEN** the two items are selected and the base is not, and the item reached through its reflection takes a grab at the reflected centre with the reflected displacement — where selecting on the mirror-expanded bound took the base as well and gave that item a grab two diameters off its body

#### Scenario: Material under the ball moves even when it is a copy
- **WHEN** a mirrored drag is applied over an item and over another item's reflection
- **THEN** the material under the ball moves by the same amount whether it is an item or a copy, and so do both of their reflections

#### Scenario: A mirrored drag is the mirror image of its mirror image
- **GIVEN** a mirrored layer with an identity transform and no item opted out of the mirror
- **WHEN** a drag is applied to one document and its reflection across the plane to a fresh one
- **THEN** the two documents carry identical deformer chains and evaluate to identical fields at every sample, bit for bit

#### Scenario: An item both images reach takes one grab per image, in a fixed order
- **WHEN** a drag's ball and its reflection both reach an item straddling the plane
- **THEN** the item takes one warp carrying one grab per image, the two pulls compose as two brushes would, and the grabs are ordered by value so the +x drag and its mirror image produce the same field whether or not the item is itself plane-symmetric

#### Scenario: An opted-out item sees the ball, not its reflection
- **WHEN** an item that does not participate in the mirror sits only where the reflected ball would reach it
- **THEN** it takes no warp, and the same item participating takes one through its copy

#### Scenario: Two mirror axes make two reflections, not four quadrants
- **WHEN** a layer mirrors about two axes and a drag is resolved in one quadrant
- **THEN** the items in the two single-reflection quadrants are selected and the item in the diagonal quadrant is not

#### Scenario: A prepared drag under symmetry is the one-step drag
- **WHEN** a drag on a mirrored, two-axis or radial layer is prepared once and resolved for a displacement
- **THEN** every item's grabs and the rest of its gesture identity are bit-identical to resolving the drag in one step, and an item reached only through its copy is prepared with that image marked as the one that reaches it

#### Scenario: Radial symmetry rotates the brush the same way
- **WHEN** a layer carries a radial count and an item's rotated copy sits under the ball
- **THEN** that item takes a grab at the ball rotated by the copy's inverse angle, the copy under the ball moves, and the rotated-image drag on a fresh document matches the original to floating-point tolerance

### Requirement: The Move brush inherits grab's pull, and says so
The move SHALL use the existing `grab` deformation rather than a new one, and SHALL therefore inherit its behaviour: the surface moves LESS than the displacement asked for, because the region weight is taken at the sample point rather than at its preimage.

This SHALL be documented rather than corrected. Solving for the true preimage costs an iteration per sample and buys nothing a sculptor can feel, and the pull is monotonic in the displacement, so a UI can calibrate against it.

#### Scenario: The pull is monotonic
- **WHEN** the same drag is resolved at increasing displacements
- **THEN** the surface moves further each time, by less than the displacement given

### Requirement: A drag coalesces rather than accumulating
A Move applied repeatedly as one gesture SHALL replace that gesture's warps rather than adding others beside them. A drag holds its centre and radius fixed and grows only its displacement, so those two identify the gesture without a caller having to thread an identifier through — under symmetry, the centre and radius of each image of the gesture on that item, whether or not the image reaches the item this frame.

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

#### Scenario: An item on the mirror plane does not stack its two grabs
- **WHEN** a mirrored Move whose ball and reflection both reach an item is applied over several frames
- **THEN** the item carries exactly two grabs after every frame, and the field equals a single application of the final displacement

#### Scenario: An image that starts or stops reaching mid-drag is still the same gesture
- **WHEN** a frame's pull widens an item's bound so that a second image reaches it, or a later frame's smaller pull lets that image miss it again
- **THEN** the chain holds one grab per image reaching the item that frame and never two grabs of one identity, because every leading grab matching any image of the gesture is replaced

### Requirement: A move can be previewed without applying it
The library SHALL expose which items a drag would warp without modifying the document, so a host can preview a Move, or show what it is about to affect, before committing it.

#### Scenario: Previewing changes nothing
- **WHEN** a drag is previewed
- **THEN** the document evaluates identically to before, and the preview names the items the move would warp

#### Scenario: The preview agrees with the move
- **WHEN** a drag is previewed and then applied
- **THEN** the items reported by each are the same

### Requirement: A stamp's strength scales the amount, where the op has one
Turning stamps into edit-list items SHALL apply a stamp's strength to the item's amplitude for the ops whose `blend.k` IS an amplitude — relief and incise — so that the stroke engine's pressure-strength channel and its accumulation mode reach an SDF layer.

For add, the amount IS the stamp: strength SHALL scale the whole deposit — the item's scale and its blend radius together, with the rounding following the scale — so a stroke at strength zero authors no node at all, full strength authors the item exactly as it would have been without a strength, and the deposited displacement is monotonic in between. The clamped-accumulation division SHALL NOT apply to an add stamp's deposit: overlapping unions do not add up, so dividing by the overlap would shrink every stamp of a clamped stroke rather than keep the stroke at its strength.

For every other op `blend.k` is a radius, a depth or a half-thickness, and no scale is purely an amount. Scaling either by a stroke's strength would change the SHAPE rather than the amount, differently per op and silently, so those ops SHALL continue to ignore strength.

#### Scenario: Buildup accumulates past clamped
- **WHEN** the same dense relief stroke is applied under buildup and under clamped accumulation
- **THEN** the buildup pass moves the surface further

#### Scenario: A lighter touch deposits less
- **WHEN** a relief stroke is applied with reduced pressure strength
- **THEN** the surface moves less than at full strength

#### Scenario: An add stroke honours strength
- **WHEN** the same add stamp is applied at strengths 0, 0.1, 0.5 and 1.0
- **THEN** strength 0 leaves the layer unchanged, 1.0 deposits the item exactly as authored, and the surface displacement rises monotonically in between

#### Scenario: A clamped add stroke keeps its deposit
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

### Requirement: Stamps apply to a mesh
The stroke engine SHALL gain `apply_to_mesh`, a fourth consumer of `resolve_stroke`'s stamps alongside `apply_to_grid`, `apply_to_mask` and `stamps_to_nodes`.

It SHALL take a mesh, its adjacency, the resolved stamps, the verb and its settings, and SHALL apply one stamp per resolved stamp, so spacing, pressure response, deterministic jitter, taper, steady stroke and buildup-versus-clamped accumulation reach mesh sculpting with no new machinery.

Each stamp's world radius and strength SHALL come from the stamp rather than from the settings, so pressure and taper shape a mesh stroke exactly as they shape a voxel one.

Under `Buildup` accumulation, overlapping stamps SHALL act repeatedly; under `Clamped`, the stroke SHALL reach its strength once however many stamps overlap. This is what makes one `clay` stamp into ClayBuildup.

`snakehook` SHALL derive its per-stamp displacement from the motion between consecutive stamps, so a drag is a drag rather than a repeated identical pull. `grab` SHALL derive its displacement from the motion since the stroke BEGAN, applied to the region it captured — see "A grab carries the region it captured" — because a grab is one gesture moving one piece of surface and not a sequence of independent nudges.

It SHALL return the number of stamps that actually moved a vertex, and SHALL report the accumulated vertex deltas for the whole call as one coalesced record when the caller asks for it.

#### Scenario: A stroke inherits the engine
- **WHEN** a stroke is resolved with taper, jitter and a pressure curve and applied to a mesh
- **THEN** the stamps that reach the mesh are exactly the ones `resolve_stroke` produced, at their radii and strengths

#### Scenario: One stroke is one undo step
- **WHEN** a whole stroke is applied with a delta record requested and the record is reverted
- **THEN** the mesh is bit-identical to its pre-stroke state

### Requirement: A mask gates a mesh stroke
`apply_to_mesh` SHALL take a `voxel::MaskField` the same way `apply_to_grid` does: a stamp centred in a fully masked region SHALL be dropped, and each vertex's weight SHALL be scaled by `1 - mask` sampled at that vertex's world position.

The mask SHALL be sampled per VERTEX rather than per stamp, so a half-masked region under one stamp moves on one side and not the other.

This SHALL hold for every verb, including `grab` and `snakehook`, with no per-verb code.

#### Scenario: Half a region is protected
- **WHEN** the same stroke crosses a region whose far half is fully masked, for a displacement verb and for `smooth`
- **THEN** only the unmasked vertices moved, and the masked ones are bit-identical

### Requirement: Mesh strokes stay out of the tape
Nothing in `apply_to_mesh` SHALL enter a tape, a document evaluation, an edit list or the parity fixture. A sculpted mesh layer SHALL still never be evaluated, never blend with a field, and export exactly as its vertices say.

#### Scenario: The document evaluates to the same field
- **WHEN** a mesh layer in a document is sculpted
- **THEN** the document's evaluated field at every sample is unchanged

### Requirement: A world-placed cage resolves into per-item lattices
The brush engine SHALL resolve one world-placed lattice cage into the per-item lattice deformers that reproduce it, so a gizmo cage acts on a layer's assembled form rather than on one item in its own frame.

This SHALL follow the resolver pattern the Move brush established: the layer is READ and never written, the warps are RETURNED rather than applied so a host can preview and so one command per node inside an undo group makes the gesture one undo step, and each warp SHALL belong at the FRONT of its node's chain, since the chain applies in authoring order and the first entry is the outermost warp.

Groups SHALL take no warp of their own; a group's transform does not reach its children in this scene model, so the children carry it.

An item's frame may be ROTATED, and a lattice box is axis-aligned by construction, so no per-item box reproduces a world-axis-aligned cage. The resolved deformer SHALL therefore carry a TRANSFORM and be exact, rather than resampling the cage onto a per-item grid, which would be an avoidable approximation.

#### Scenario: A cage over two items reaches both
- **WHEN** a cage is placed over a layer holding two items and one control point is dragged
- **THEN** both items receive a lattice warp expressed in their own frames

#### Scenario: A rotated item is warped exactly
- **WHEN** the same world cage is resolved onto an item rotated in its layer and onto an unrotated copy at the same world pose
- **THEN** both evaluate to the same world-space field

#### Scenario: An untouched cage resolves to nothing
- **WHEN** a cage whose control points have not been dragged is resolved
- **THEN** no warps are produced, because a chain of no-op deformers is worse than none

#### Scenario: Every item is reached, unlike a drag
- **WHEN** a cage is resolved over a layer holding an item far outside its box
- **THEN** that item still receives a warp, because a lattice's displacement outside its box is CLAMPED rather than zero and the material there travels rigidly

### Requirement: A lattice deformer may carry a transform
The kernel dialect SHALL provide a lattice deformer that maps the point into the cage's own space, warps it there, and maps it back — `p' = T⁻¹(T(p) + D(T(p)))` — so a cage placed anywhere in the world can be applied to an item in any frame.

It SHALL be a SEPARATE opcode from the axis-aligned lattice rather than a flag on it. The axis-aligned path SHALL pay nothing for the transformed one existing: adding per-sample work to a path that does not need it is the defect two prior changes had to undo.

The transform and its inverse SHALL both ride the blob, beside the offsets, rather than being derived per sample.

The declared Lipschitz factor SHALL be the one the untransformed cage reports, and the spec states why rather than leaving it to be rederived: the transform is rigid with uniform scale, so with `T = sR` the warp's Jacobian in the item's frame is `R⁻¹ J R`, similar to the cage-space Jacobian and therefore of the same norm.

The influence bound SHALL be grown by the largest control-point offset divided by the transform's scale, since a displacement bounded by that in cage space is bounded by that over the scale in the item's frame.

#### Scenario: An identity transform is the axis-aligned cage
- **WHEN** a transformed lattice whose transform is the identity is compared to the plain lattice with the same box and offsets
- **THEN** the two fields agree at every point

#### Scenario: The transform does not change the bound
- **WHEN** the same cage is applied with and without a rotation and uniform scale
- **THEN** the reported safe step scale is the same

#### Scenario: The axis-aligned path is untouched
- **WHEN** a document using only axis-aligned lattices is evaluated
- **THEN** it costs what it did before the transformed opcode existed

### Requirement: A stamp can be placed from a surface hit
The brush engine SHALL provide the placement a host needs to stamp an alpha where a user clicked: a centre, a direction and a tangent derived from a surface point and its normal, so a host does not recompute the frame and get a different answer from the one the engine would.

#### Scenario: A placement derived from a hit faces the surface
- **WHEN** a placement is built from a surface point and its normal
- **THEN** the stamp's direction is the normal and its tangent is perpendicular to it

### Requirement: A mesh dab costs what it moves
Applying a mesh brush stamp SHALL cost in proportion to the geometry it AFFECTS, not to the size of the mesh, whenever the mesh carries a spatial index the operation can consult.

The brush verbs already have this property — they iterate the region and nothing else. The requirement exists because the bookkeeping AROUND them did not: per-class arrays cleared per stamp, a seed found by scanning every class, and a region found by scanning every class. A stamp SHALL NOT clear or scan an array proportional to the mesh in order to record or find something proportional to the brush.

Where a per-class array is needed — the verbs index one by arbitrary ring neighbours — it SHALL be sized once and reset through the list of entries actually written, never cleared wholesale.

#### Scenario: The same dab costs the same on a bigger mesh
- **WHEN** a stamp affecting a fixed number of weld classes is applied to meshes of increasing size, on a mesh whose spatial index exists
- **THEN** the cost is flat rather than growing with the mesh

### Requirement: The brush uses the ray tree it finds and never builds one
Where a mesh brush needs a spatial query — the region under the brush, or the seed a surface walk starts from — it SHALL use the mesh's ray tree when one exists, and SHALL fall back to a scan when one does not. It SHALL NOT build a ray tree on its own behalf.

This is a measured decision rather than a stylistic one: building a tree costs 689 ms on a million-vertex mesh and saves about 1.24 ms per stamp, so a brush that built one would need some five hundred stamps to break even and would make every shorter session worse. A host that places its brush by picking already owns a tree, so the common case is served for free.

The indexed path and the fallback SHALL produce the SAME region, so a brush cannot behave differently according to whether the host happened to have picked. Where the two could differ only by the order in which a set was collected, the order SHALL be made canonical rather than left to the tree's shape — a rebuild changes that shape, and the verbs accumulate float sums over the region.

#### Scenario: A host that never picks is unaffected
- **WHEN** a stamp is applied through a sculptor that has never built a ray tree
- **THEN** the result is identical to the same stamp through a sculptor that has one

#### Scenario: The tree is current before it is consulted
- **WHEN** a region query consults the ray tree after earlier stamps have moved vertices
- **THEN** the tree is refitted first, so the region is the set of vertices that are under the brush NOW

### Requirement: A brush is composed from orthogonal policies
The library SHALL express a brush as a composition of named, independent axes rather than as a verb with a settings struct: a stroke preset, a footprint, a weight model, a reference frame, a deformation kernel, an accumulation rule, a write target and a post policy.

Artist-facing brush families — ClayBuildup, DamStandard, hPolish, Trim Dynamic, Snake Hook, Rake — SHALL be expressible as values of those axes and SHALL NOT require an engine path of their own. A new named brush that needs a new code path is evidence an axis is missing, and the axis is what SHALL be added.

The axes SHALL be enums and plain data, not polymorphic objects. A per-vertex loop that dispatches virtually cannot be specialized, serialized or mirrored across a C ABI, and all three are required of this model.

The reference frame SHALL be explicit. `draw` displacing along the region's averaged normal and `inflate` along each vertex's own normal SHALL be the same kernel under two frames, and the results SHALL be identical to the two separate verbs they replace.

#### Scenario: A named brush family is a preset
- **WHEN** a preset library entry for a named brush family is loaded and applied
- **THEN** it resolves to axis values over existing kernels, and no kernel exists whose only caller is that one family

#### Scenario: Draw and inflate remain distinct under one kernel
- **WHEN** the same kernel is applied under the region-normal frame and under the vertex-normal frame to a region straddling a saddle
- **THEN** the two results differ in the way `draw` and `inflate` differed before this change, bit for bit

### Requirement: A stroke compiles its preset once
The library SHALL validate and compile a brush preset into a flat runtime plan at the beginning of a stroke, and each stamp SHALL read that plan rather than re-inspect the preset.

The plan SHALL record what the kernel actually needs — normals, neighbours, a pre-stamp snapshot, an alpha sampler, automask factors — so that a stamp gathers only what it will use.

A plan SHALL be cached against the preset's revision, and an unchanged preset SHALL NOT be recompiled between stamps.

#### Scenario: A plan is compiled once per stroke
- **WHEN** a stroke of many stamps runs with an unchanged preset
- **THEN** the preset is compiled exactly once and every stamp reads the compiled plan

### Requirement: Brush behaviour does not depend on event batching
Resolving the same stroke path SHALL produce the same stamps whether the host delivers its samples in one batch or in several, provided the stroke's transaction state is retained between calls.

Deterministic jitter, spacing, taper and clamped accumulation SHALL all hold under that split, because a host's event coalescing is a property of the device and not of the artist's gesture.

#### Scenario: One batch and five batches agree
- **WHEN** a stroke path is resolved as one batch of samples and then as five consecutive batches through one transaction
- **THEN** the resolved stamps are identical

### Requirement: Automasking gates a brush without a per-verb branch
The library SHALL provide automask factors — normal angle, topology connectivity, boundary proximity, cavity, and surface-group membership — evaluated over the brush's WORKSET and composed into the per-vertex weight by multiplication.

An automask SHALL be computed for the vertices a stamp reaches and SHALL NOT be computed, allocated or scanned for the whole surface.

Cavity and curvature automasks SHALL be derived from the SAME estimator the procedural mask verbs use, so that a painted cavity mask and a cavity automask cannot disagree about one surface.

The cavity and surface-group automasks read WORLD-ADDRESSED lattices, and a sculptor's vertices are not in world space. Each vertex SHALL be placed by the stroke's mesh-to-world transform before either lattice is sampled — the same placement the painted mask already receives. One estimator does not prevent two answers about one surface if its two callers ask it about different points.

The surface-group automask SHALL read the document's group field rather than a per-face group identifier. Groups are addressed on a world lattice so that they survive a representation bridge; a per-face copy would be a second answer to the same question and would not survive one.

The cavity strength SHALL be honoured at zero. Zero is off even when the factor's bit is set, because it is what a host's slider at zero means, and a binding that reads it as "unset, take the default" inverts it.

A fully automasked vertex SHALL be bit-identical to its input position.

#### Scenario: A cavity automask and a painted cavity mask agree
- **WHEN** a cavity automask and a procedural cavity mask are evaluated over the same surface with the same parameters
- **THEN** they report the same values within the estimator's own tolerance

#### Scenario: An automask costs the workset, not the model
- **WHEN** a stamp with every automask enabled runs on a mesh of a million vertices with a footprint of a few thousand
- **THEN** the automask evaluation touches the workset and its read halo only

#### Scenario: A transformed layer's automask reads where the layer is
- **WHEN** a stroke on a layer whose transform is not the identity enables the cavity or surface-group automask
- **THEN** the lattice is sampled at the vertex's world position, and the factor gates the same surface a painted mask on that layer gates

#### Scenario: A cavity strength of zero is off
- **WHEN** a brush sets the cavity factor's bit and a cavity strength of zero
- **THEN** the factor scales no weight, and the stamp is the stamp it would have been with the factor off

### Requirement: A brush preset is versioned and carries no image data
A brush preset SHALL carry a schema version from its first release, SHALL deserialize an older version by supplying defaults, and SHALL REFUSE an unknown newer version rather than interpreting the prefix it recognises.

A preset SHALL NOT embed alpha, height or displacement image bytes. Image content SHALL remain caller-owned and borrowed for the duration of a call, as the mesh alpha stamp already requires, so that a preset library costs kilobytes and a host owns its own resource cache.

#### Scenario: A newer preset is refused
- **WHEN** a preset serialized by a newer schema version is deserialized
- **THEN** the call fails with a typed error and produces no partially populated preset

#### Scenario: A preset round-trips
- **WHEN** a preset is serialized, deserialized and used to resolve a stroke
- **THEN** the resolved stamps are identical to those from the original preset

### Requirement: A world magnify resolves into a field-level radial scale
A magnify stated in WORLD space — a centre, a radius and a SIGNED strength — SHALL resolve into one `magnify` deformer per item the region reaches, each already in that item's own frame, so that the layer's ASSEMBLED surface swells or gathers rather than one item's share of it.

`magnify` is per item and applied to that item's local point, exactly as `grab` is, so a magnify put on one item of a smooth-unioned form scales that item's field and leaves the others where they were. This is the hazard `move_brush` exists for, and it applies to the radial scale verbatim.

A POSITIVE strength SHALL swell the surface away from the centre and a NEGATIVE one gather it toward. One signed parameter covers Magnify and Pinch, which are one deformation.

The strength SHALL cross the layer's symmetry images unchanged: a reflection or a rotation of a radial scale is a radial scale of equal strength, unlike a drag's displacement, which has to be mapped per image.

This SHALL follow the resolver pattern the Move brush established: the layer is READ and never written so a host can preview the gesture, the warps are RETURNED rather than applied so one command per node inside an undo group makes the gesture one undo step, and each warp SHALL belong at the FRONT of its node's chain.

Items the region cannot reach SHALL take no deformer, a strength of zero SHALL produce nothing, and a non-positive radius SHALL produce nothing.

#### Scenario: A blended form swells as one surface
- **WHEN** a magnify is resolved over a form smooth-unioned from two items and the warps are applied
- **THEN** both items take a share and the surface swells symmetrically about the gesture's centre

#### Scenario: Magnifying one item is not the same thing
- **WHEN** the same deformation is expressed as a magnify on a single item instead
- **THEN** that item's side moves and the other is left behind

#### Scenario: The sign chooses Magnify or Pinch
- **WHEN** the same region is resolved at a positive and then a negative strength
- **THEN** the surface swells away from the centre in the first case and gathers toward it in the second

#### Scenario: A transformed layer maps correctly
- **WHEN** the layer carries a transform and a magnify is resolved in world space
- **THEN** the surface changes where the gesture was aimed, and the radius each item sees is the world radius through that layer's scale

#### Scenario: Nothing is written
- **WHEN** a magnify is resolved
- **THEN** the document is unchanged until the caller applies the result

### Requirement: A magnify gesture coalesces rather than accumulating
A magnify applied repeatedly as one gesture SHALL replace that gesture's deformers rather than adding another beside them, for the reason a drag does: otherwise the chain grows by one entry per frame and each entry multiplies into the declared Lipschitz.

A gesture SHALL be recognised by its KIND as well as by its centre and radius. A drag and a magnify over the same ball are two gestures, and neither SHALL replace the other's leading deformer.

#### Scenario: A live gesture replaces its own last frame
- **WHEN** a magnify is applied over several frames at a growing strength
- **THEN** each item carries one magnify, and the document is the one a single frame at the final strength produces

#### Scenario: A pinch does not swallow a drag over the same ball
- **WHEN** a magnify is applied over a ball that already carries a drag's grab
- **THEN** both remain, the magnify in front

### Requirement: A magnify can be previewed without applying it
Resolving SHALL be pure, and a caller SHALL be able to learn which nodes a magnify would warp without touching the document. The preview SHALL refuse exactly what the apply refuses, so a host does not discover a malformed gesture only on commit.

#### Scenario: The preview names what the apply touches
- **WHEN** a magnify is previewed and then applied over the same region
- **THEN** the preview names the same nodes, and the document is unchanged until the apply

### Requirement: A drag resolves in two halves
Resolving a world drag into per-item warps SHALL be separable into the half that does not depend on how far the drag has gone and the half that does.

A drag holds its anchor and its radius fixed for the whole gesture and grows only its displacement. Everything that follows from the anchor and the radius — which items the drag reaches, where its centre lands in each item's own frame, what its radius becomes there — SHALL therefore be resolvable ONCE, and turning that into a warp for a given displacement SHALL then cost no scene access at all. Without the split, a live drag walks the whole edit list once per pointer event to rediscover an answer that cannot have changed.

The two halves composed SHALL be BIT-identical to resolving the drag in one step, for every item, for every displacement, including under a transformed layer, a rotated item, a per-axis scale, a non-default falloff, the front-only gate and items nested in groups. A preview whose commit differs in the last bits is a preview of something else, so the prepared form SHALL keep the terms the one-step form divides and rotates by rather than pre-inverted equivalents — a reciprocal multiplied is not a division.

Preparation SHALL be able to report what it walked — the nodes visited and, of those, the items the drag can reach — so that the property "the traversal is paid once" is testable as a counter rather than as a duration.

The refusals SHALL be unchanged by the split: a non-positive radius and a layer with no edit list SHALL prepare nothing, and an item the drag cannot reach SHALL receive no warp.

#### Scenario: Prepared and resolved is the same warp
- **WHEN** a drag is prepared once and resolved for each of several displacements
- **THEN** each resolved warp names the same node and carries the same deformer, bit for bit, as resolving that drag in one step

#### Scenario: The traversal happens once
- **GIVEN** a layer holding thousands of items of which a drag reaches two
- **WHEN** the drag is prepared and then resolved
- **THEN** preparation reports having visited every node and reached two, and resolving visits neither

#### Scenario: A radius that is not a drag prepares nothing
- **WHEN** a drag is prepared with a non-positive radius
- **THEN** nothing is prepared

### Requirement: The chain ordering rule applies to a chain a caller holds
The rule that places a drag's warp at the FRONT of an item's chain, and replaces a leading warp from the same drag rather than stacking another beside it, SHALL be expressible against a deformer chain held by value as well as against a node in the document.

A live gesture must hold the pre-stroke chain by value, because the node in the document is the one thing it has promised not to touch. Both forms SHALL be the same rule — the node form SHALL be the chain form applied to that node's chain — so a caller cannot get the ordering subtly wrong by holding its own copy.

#### Scenario: The two forms agree
- **WHEN** the ordering rule is applied to a node and to a copy of that node's chain, with the same warp
- **THEN** the two chains are identical deformer for deformer

#### Scenario: The coalescing rule travels with it
- **WHEN** the rule is applied twice with the same drag's warp to a chain held by value
- **THEN** the chain does not grow the second time

### Requirement: Every part of a brush reaches every representation that offers the verb
Where a representation offers a verb, it SHALL honour EVERY factor of the brush that composes into the per-vertex weight — the falloff, the path taper, the freeze, the alpha, and the automask — and SHALL apply them in the one fixed order the composition rule states.

A representation that cannot honour a factor SHALL decline the verb, the way an adaptive surface declines Layer because half the vertices under the brush at the end of a stroke did not exist at its start. It SHALL NOT accept the brush and silently drop the factor. A brush setting that is accepted and ignored is worse than one that is refused: an artist who enables a gate and sees the surface move anyway has no way to tell a disabled gate from a gate that decided not to fire.

This requirement is written because the library did the ignored thing. The adaptive sculptor took a brush carrying automask settings, composed four of the five factors and dropped the fifth, on a path whose own C ABI documentation states that the descriptor is "the same descriptor the fixed path takes, so a host carries one brush model across both representations".

#### Scenario: An automask reaches the adaptive surface
- **WHEN** a stamp with the normal-angle automask enabled is applied to an adaptive surface across a fold
- **THEN** it moves fewer vertices than the same stamp with the automask disabled, and the vertices it declines to move are the ones facing away from the brush

#### Scenario: A factor that cannot be honoured is refused, not dropped
- **WHEN** a brush carries a factor a representation cannot compute
- **THEN** the call is refused with a reason, and no stamp is applied

### Requirement: A directional brush family is a preset over the shared frame
Rake, chisel, clay strips, a directional scratch and a rotated alpha SHALL be expressible as axis values over the shared stamp frame, and SHALL NOT require a frame, a sampler or a code path of their own.

The stamp's azimuth SHALL be part of the brush's settings, so that a stroke resolver which knows the direction of travel can orient the stamp without any verb knowing that it did.

The azimuth SHALL be carried by the brush preset format, at a schema version, as part of the brush's identity rather than of where a stamp landed. Nothing resolves an azimuth from a stroke's direction of travel yet, so a preset is the only place an artist can put one, and a library that dropped it would give a turned brush back unturned — the failure a version number exists to prevent. A record written by an earlier schema SHALL still load, taking the unrotated default it was in fact saved with.

#### Scenario: A directional family needs no engine path
- **WHEN** a directional preset is applied
- **THEN** it resolves to an azimuth on the shared stamp frame over an existing kernel, and no kernel exists whose only caller is that family

#### Scenario: A turned brush stays turned across the format
- **WHEN** a preset carrying a non-default azimuth is serialized and read back
- **THEN** it reports the same azimuth, and a preset carrying the default reports an exact positive zero

#### Scenario: A record from the earlier schema still loads
- **WHEN** a preset record written before the azimuth was carried is read
- **THEN** it loads with the unrotated default rather than being refused, while a record that is also truncated is still refused

### Requirement: A host can budget the memory a stroke's scratch will ask for
The library SHALL report, per sculptor, the capacity its scratch arena currently holds, the largest it has ever held, and how many times it has grown.

The reason is the same one the hierarchy's preflight gives: the device this library targets kills an application for memory rather than warning it twice, so a host that can see what a stroke's scratch costs can budget for it, and a host that cannot is guessing.

The library SHALL NOT expose a tuning knob for that arena — no reserve, no cap, no growth factor. Each would be a number a host tunes against one device and is then wrong about after a footprint change, and the arena already sizes itself from the largest footprint it has actually seen, which is the measurement such a knob would be guessing at.

#### Scenario: The scratch cost is reportable
- **WHEN** a host queries a sculptor after a stroke
- **THEN** it receives the arena's capacity, high-water mark and growth count

#### Scenario: There is nothing to tune
- **WHEN** a host looks for a way to preallocate or cap the arena
- **THEN** the API offers none, and the arena's capacity follows the footprints it has been given

### Requirement: A stroke does not re-find an anchor it already has

A geodesic region is grown from an ANCHOR class, and finding that anchor without
a spatial index costs a scan over every class in the surface. A sculptor SHALL
NOT pay that scan once per dab: after a stamp has resolved an anchor, the next
stamp of the same stroke SHALL start from it rather than searching the model
again. This is what makes a stamp on a subdivision level cost what it touches —
the hierarchy builds no ray tree, so without it every dab is proportional to the
level's vertex count.

**A carried anchor SHALL NOT change what a stamp produces.** The walk accumulates
its path distance from the anchor, so the anchor is an input to the falloff and
not merely a starting point for a search: substituting a different class would
move the surface. A carried anchor SHALL therefore be used to LOCATE the class
the unaccelerated search would have returned, and the positions, colours and
moved counts a stroke produces SHALL be identical to those it produced when every
dab searched from scratch.

A carried anchor SHALL be validated before use and discarded rather than trusted:
one from a retired class space, or one further from the new centre than the brush
radius, SHALL fall back to the unaccelerated search. An anchor further than the
radius makes the walk return an EMPTY region, so an unvalidated one does not
misplace a dab, it silently loses it — indistinguishable to a host from a fully
masked stroke.

#### Scenario: A stroke's dabs do not each search the model
- **WHEN** a stroke of many dabs is applied to a surface with no spatial index, at a footprint far smaller than the surface
- **THEN** the classes examined to anchor each dab after the first follow the footprint rather than the surface's size

#### Scenario: An accelerated stroke and a cold one agree exactly
- **WHEN** the same stroke is applied twice, once where every dab searches from scratch and once where each dab starts from the previous dab's anchor
- **THEN** the two surfaces are identical — positions, colours and moved counts — rather than close

#### Scenario: An anchor out of reach is discarded, not spent
- **WHEN** a dab lands further from the carried anchor than the brush radius
- **THEN** the anchor is discarded and the dab moves the vertices it would have moved with no anchor at all, rather than moving nothing

### Requirement: The automask sources read the frame their own handle declares

The inputs that make `CLAY_AUTOMASK_CAVITY` and `CLAY_AUTOMASK_SURFACE_GROUP`
answer SHALL be asked about the point the surface actually occupies, on every
sculptor that can carry them.

#### Scenario: Each sculptor places its own sources

- **GIVEN** automask sources named through
  `clay_mesh_sculptor_set_automask_sources`,
  `clay_dynamic_sculptor_set_automask_sources` or
  `clay_multires_sculptor_set_automask_sources`
- **WHEN** either factor is consulted during a stamp
- **THEN** the cavity field and the group lattice SHALL be sampled at the point
  placed by the frame that sculptor declares

#### Scenario: The frame is read when the lattice is asked

- **GIVEN** a host that names its sources BEFORE declaring its frame
- **WHEN** it then declares one and stamps
- **THEN** the lattices SHALL be asked at the placed point, because the frame is
  read at the moment the lattice is consulted and not captured when the sources
  were named

#### Scenario: One transform, not two

- **GIVEN** a host that both names sources through the C setter and drives a
  stroke that carries its own cavity field
- **WHEN** the second set of inputs is installed
- **THEN** it SHALL REPLACE the first rather than compose with it, so the point
  is placed once and not twice

### Requirement: Stamps apply to an adaptive surface
The stroke engine SHALL provide `apply_to_dynamic`, a consumer of `resolve_stroke`'s stamps alongside `apply_to_mesh` and `apply_to_multires`, so spacing, pressure response, deterministic jitter, taper, steady stroke, accumulation, the stylus azimuth and brush presets reach an adaptive surface with no new machinery.

It SHALL apply exactly one `DynamicSculptor::stamp` per resolved stamp, with each stamp's radius and strength taken from the stamp and the rest of the brush from the settings — the same resolution of a stamp into brush settings that the fixed and multiresolution consumers use, shared rather than copied.

`grab` SHALL carry the region it captured at its first stamp and displace it by the motion since the stroke began, as `apply_to_mesh` does — see "A grab carries the region it captured" — with the remesh maintaining that region and each stamp's remesh run at that stamp's own centre once the region is captured, per "An adaptive surface maintains a carried region rather than losing it". `snakehook` SHALL centre every stamp on the surface VERTEX it is dragging, so the brush keeps up with the pull.

Because the adaptive surface retires vertex identities when it collapses an edge, a `snakehook` stroke SHALL revalidate its anchor before every stamp and, when the anchor no longer exists, SHALL re-find it as the vertex nearest the previous stamp's position, using the same nearest-vertex estimator the sculptor seeds its walk with. A stroke SHALL NOT keep stamping at a retired anchor's last position: measured, that reaches only 15–18% of a pull-out where the anchor dies often, and 41–88% where it dies one to three times.

A mask SHALL gate the stroke as it gates the other mesh consumers: a stamp centred in a fully masked region SHALL be skipped, and each vertex SHALL be weighed by the mask at its placed position. The cavity and surface-group estimators in the stroke options SHALL be wired once for the stroke. With `orient_alpha_by_stamp` set, each stamp's rotation SHALL orient the alpha; without it the alpha's tangent SHALL be exactly what the caller set.

It SHALL refuse, applying nothing and touching neither the surface, the record nor the sculptor's automask inputs: `MeshBrush::Layer`, which an adaptive surface does not offer; and a request to defer normals, which the adaptive sculptor cannot honour. A refusal SHALL NOT be a silent remap to another verb or a silently ignored option.

With a topology record given, the whole call SHALL accumulate into it as one reversible gesture. It SHALL return the number of stamps that changed the surface, and SHALL accumulate, when asked, the moved vertices, the topology operations, whether any stamp hit its operation budget, and the union of the dirty bounds.

#### Scenario: A stroke equals its stamps
- **WHEN** a stroke with taper, jitter and a pressure curve is applied to an adaptive surface through `apply_to_dynamic`, and the same resolved stamps are applied one by one through `DynamicSculptor::stamp` with the same per-stamp settings to an identical surface
- **THEN** the two surfaces are bit-identical, topology included, and the applied counts agree

#### Scenario: A snakehook keeps pulling
- **WHEN** a snakehook stroke pulls away from an adaptive surface
- **THEN** the surface follows the drag the way the same stroke through `apply_to_mesh` follows it on a fixed mesh, and not the way a loop centring each stamp on the cursor falls behind it

#### Scenario: A retired anchor is re-found
- **WHEN** the remesher retires the vertex a snakehook stroke is dragging
- **THEN** the next stamp re-finds the anchor nearest the previous stamp's position and the stroke keeps pulling, rather than stamping where the retired vertex was

#### Scenario: Layer is refused, not remapped
- **WHEN** a Layer stroke is applied to an adaptive surface
- **THEN** no stamp is applied, the surface and the record are unchanged, and the call reports zero

#### Scenario: The azimuth reaches the alpha on request
- **WHEN** two strokes that differ only in stylus azimuth are applied with an asymmetric alpha and a preset that turns the stamp to the barrel
- **THEN** with `orient_alpha_by_stamp` the surfaces differ, and without it they are bit-identical

#### Scenario: One stroke is one undo step
- **WHEN** a whole adaptive stroke is applied with a topology record and the record is reverted
- **THEN** the surface is bit-identical to its pre-stroke state and validates

### Requirement: The estimators a mesh module cannot compute are set once per stroke and wired by every stroke resolver
The callbacks a mesh module structurally cannot compute for itself — the cavity measure, which is a field's Laplacian, and the surface-group field, which is a world lattice — SHALL be settable on EVERY sculptor that offers the automask, with the same signature, and SHALL be set once per STROKE rather than per stamp.

Per stroke is not a preference. They hold callable objects, and copying those per dab is an allocation per dab, which the allocation discipline forbids.

A sculptor that composes another — a multiresolution sculptor over a level sculptor — SHALL forward them, including to a level bound after they were set, so that changing the sculpt level mid-stroke does not silently drop them.

A STROKE RESOLVER THAT DRIVES A SCULPTOR SHALL WIRE THEM. `brush::apply_to_mesh`, `brush::apply_to_multires` and `brush::apply_to_dynamic` wire them from `MeshStrokeOptions`, once per call. A host that drives any sculptor stamp by stamp instead sets them on the sculptor itself, with the same call on all three. Where the stroke options carry no estimator, the resolver SHALL leave whatever the host set on the sculptor in place rather than clearing it.

#### Scenario: Every sculptor takes the estimators
- **WHEN** a host sets the cavity and group estimators on the fixed, adaptive and multiresolution sculptors
- **THEN** all three accept them through the same call, and a stamp on each applies the cavity and surface-group factors

#### Scenario: Setting them allocates nothing per dab
- **WHEN** a stroke of many stamps runs after the estimators were set once
- **THEN** no stamp allocates on their behalf

#### Scenario: The adaptive stroke resolver wires them
- **WHEN** a host applies a stroke to an adaptive surface through `brush::apply_to_dynamic` with a cavity estimator in the stroke options
- **THEN** every stamp of the stroke applies the cavity factor, exactly as the same options do through `apply_to_mesh`

### Requirement: An adaptive stroke records into a replayable gesture
The brush engine SHALL offer a recorded sibling of the adaptive stroke consumer that captures the whole stroke into a replayable gesture — the topology delta together with the surface marks at both ends — so that one stroke, with the stroke's own drag anchoring and remesh schedule, is one undo step the sculptor's guarded replay accepts.

The surface mark SHALL be checked ONCE, before the first stamp: a non-empty record whose end is not the surface's current state SHALL be refused with nothing stamped, no revision advanced and the record unchanged. The refusals of the unrecorded consumer (a Layer verb, a request to defer normals, no stamps) SHALL be decided before the mark check and SHALL also leave the record unchanged. A mark check between stamps SHALL NOT be added while every write inside the stroke is a sculptor stamp into the same record, because such a check cannot fire.

A non-empty record whose end is the current state SHALL be continued, so stamps and strokes captured in sequence revert and apply as one step.

The surface a recorded stroke produces SHALL be bit-identical to the unrecorded stroke's with the same inputs, and the record SHALL be the same size as the one the same stamps produce when captured one by one with the stroke's rules.

#### Scenario: A recorded stroke undoes and redoes exactly
- **WHEN** a Draw, Clay, Smooth, Flatten, Grab or Snakehook stroke is applied through the recorded consumer with the relax pass on, then the record is reverted and applied through the sculptor
- **THEN** after the revert the export and the stored normals of every live element equal the pre-stroke surface bit for bit, after the apply they equal the post-stroke surface, the surface validates at both ends, and the sculptor's index covers every live face

#### Scenario: A Snakehook whose anchor dies records exactly
- **WHEN** a recorded Snakehook stroke runs on a coarse detail where the remesher retires the dragged vertex at least once
- **THEN** reverting and applying the record reproduce the pre- and post-stroke surfaces exactly

#### Scenario: Recording does not change the stroke
- **WHEN** the same stamps are applied to identical surfaces by the recorded and the unrecorded consumer
- **THEN** the surfaces are bit-identical, the applied counts and summaries are equal, and the record's encoded size equals that of the same stamps captured one by one with the stroke's rules

#### Scenario: A stale record refuses the whole stroke
- **WHEN** a record captured on a surface is followed by an unrecorded stamp, and a stroke is then applied into that record
- **THEN** the call reports the mismatch, no stamp is applied, and the record's size and both marks are unchanged

#### Scenario: A refused stroke leaves the record untouched
- **WHEN** a Layer stroke, a stroke asking to defer normals, or an empty stroke is applied into a non-empty record
- **THEN** the call applies nothing and the record's size and both marks are unchanged

#### Scenario: A refused stroke into a stale record is a refusal, not a mismatch
- **WHEN** a Layer stroke, a stroke asking to defer normals, or an empty stroke is applied into a record the surface has moved away from
- **THEN** the call reports the stroke's own refusal rather than a mismatch, applies nothing, and the record's size and both marks are unchanged

#### Scenario: Strokes accumulate into one step
- **WHEN** a recorded stamp and two recorded strokes are captured into one record and the record is reverted once
- **THEN** the surface equals the one before the stamp

### Requirement: A grab carries the region it captured
A `grab` stroke SHALL gather its region ONCE, at its first stamp, and SHALL carry that region — the items, the positions they were gathered at, and their falloff weights — for the whole gesture. Every stamp SHALL place each carried item at its captured position offset by its weight times the motion of the stroke SINCE ITS FIRST SAMPLE, rather than re-gathering a region around a point the surface has already left.

This SHALL hold identically on a fixed mesh, on a multiresolution level and on an adaptive surface, because it is a fact about what a grab IS and not about a representation.

Re-gathering every stamp around the first sample is what this requirement replaces, and it is replaced because the falloff weights shrink as the surface leaves the anchor: measured on a unit sphere at `cube_sphere(24)` with a brush radius of 0.3, a grab reached 41% of a 0.6 drag and 18% of a 1.5 drag, on both representations. A carried region reaches the whole drag at its centre, which is the weight-1 vertex, on both.

Centring each stamp on the CURSOR and re-gathering SHALL NOT be adopted, and the reasons are two and independent. It makes the representations disagree by a tenth of the drag, where a carried region makes them agree to within 2e-5. And it loses the surface: the vertex at the centre moves by its falloff weight rather than the whole delta, so the brush falls behind the cursor and is soon outside its own radius — measured, 11 of 26 stamps applied on a 1.5 drag, on both representations, after which the stroke moves nothing at all.

#### Scenario: A grab reaches the whole drag on both representations
- **WHEN** a grab stroke pulls a distance away from a fixed mesh and the same stroke pulls away from an adaptive surface built from the same model
- **THEN** each surface's furthest travel along the drag is the whole drag, and the two representations agree to within 1e-3

#### Scenario: The reach is the rule's and not the fixture's
- **WHEN** the same grab is measured with the drag's sign reversed and on the opposite pole of the same model
- **THEN** the reach is the same, rather than depending on which way the fixture happened to face

#### Scenario: A curved drag carries the same region
- **WHEN** a grab stroke follows a curve rather than a straight line
- **THEN** the captured region is carried along the whole path and arrives at the last sample — displaced by the motion from the first sample to the last, which on a curve is shorter than the path the cursor travelled — rather than being re-gathered around the first

#### Scenario: A grab is one gather, not one per stamp
- **WHEN** a grab stroke of many stamps is applied to a fixed mesh
- **THEN** the surface is walked once for the gesture rather than once per stamp, and the stroke costs less than the same stroke re-gathering every stamp

### Requirement: An adaptive surface maintains a carried region rather than losing it
An adaptive surface retires vertex identities when it collapses an edge and creates them when it splits one, and it does so BETWEEN the stamps of a gesture. A gesture carrying a captured region SHALL therefore have that region MAINTAINED by the remesh rather than left to decay: a split inside the carried region SHALL insert its new vertex into the region with the midpoint of its parents' CAPTURED positions and the mean of their weights, and a collapse SHALL remove the entry whose vertex it retired.

A new vertex's captured position SHALL be reconstructed from its parents' captured positions and SHALL NOT be read from the surface, because its parents have already taken part of the drag and reading the surface would apply that part to it twice.

A COLLAPSE SHALL NOT RETIRE A CARRIED VERTEX for the length of the gesture. Maintaining the region is necessary and is not sufficient: measured over twelve fixtures, maintenance alone keeps the region growing — nine captured entries becoming fifty-nine to sixty-four live, forty-five becoming four hundred and thirty-four to five hundred and thirty-eight — and still loses the weight that carries the drag, because the weight-one centre is collapsed in the first half of the gesture and no operation can create a weight above the one it inherits. With maintenance alone the gesture reaches between 73% and 97% of the drag and the two representations disagree by up to 0.375 in world units, which is worse than re-gathering; with the collapse refused, every one of the twelve fixtures reaches the whole drag and the two representations agree to within 1.6e-4.

Refusing those collapses SHALL NOT be treated as the remesher giving up its work, because measurably it does not: the collapses refused are the ones consuming the gesture's own region, so the region stays dense and the splits refine what the gesture stretched. Measured after a 1.5 drag, the surface left by the protected rule has a longest edge of 0.27 against 0.62 for the unprotected maintenance, 0.12 before the gesture reached that far, and 1.32 with no remesher at all; on four of six fixtures the protected rule leaves the surface exactly as fine as re-gathering does. The gesture is also CHEAPER than re-gathering — between 0.39 and 0.89 of it — because a gesture walks the surface once instead of once per stamp.

A split with exactly ONE carried parent SHALL also insert its new vertex, at the midpoint of the carried parent's CAPTURED position and the uncarried parent's CURRENT position, with half the carried parent's weight. The uncarried parent has taken no part of the drag, so where it is now is where it was captured, and the child is exactly reconstructible. This does not change the reach; it keeps the surface finer and reduces the collapses the rule above has to refuse.

A carried vertex the REMESHER ITSELF moves — a collapse placing its survivor, a relaxation sliding one along the surface — SHALL take the same shift in its captured position. Without it the next stamp writes the captured position plus its weighted share of the drag and puts the vertex back where the remesher moved it from, so the relaxation inside a gesture has no effect at all. Measured, between one and two hundred and ninety-one carried vertices are moved by the remesher on every stamp of every fixture.

A region left unmaintained is not a smaller region, it is a different answer per fixture, and on some brushes it is worse than re-gathering: measured, 7 to 13 of 45 captured vertices survived an eleven-stamp stroke, and the reach followed whether the weight-1 centre happened to be among them — the whole drag pulling one pole of a sphere and 44% of it pulling the other, with the two representations 57% of the drag apart. At a brush radius of 0.15 against a detail resolution of 4 the remesher retired ALL of the captured entries inside one stroke and the reach fell to between 2.8% and 7.0%, below the 9.3% to 22.3% that re-gathering reaches on the same fixtures. With the remesher unable to touch the region the same rule reaches the whole drag on both representations and they agree to within 2e-5.

Because no maintenance operation can create a weight above the one it inherits — a split's child takes the MEAN of its parents' weights — the maintenance SHALL be verified by the weight it preserves and not only by the entries it counts: the largest weight carried at the end of a gesture SHALL be reported alongside the entry counts, so that a region which is numerically maintained while its high-weight core has been retired is visible as a number rather than as a surface that looks about right.

A `grab` stamp's remesh SHALL run at that stamp's own centre and SHALL NOT stay at the gesture's first sample. A ball left at the first sample never reaches a tip five brush radii away, so the tip keeps the edges the drag stretched: measured after a 1.5 drag, the longest edge within a brush radius of the tip is 0.1450 with the remesh anchored against 0.0487 with it following, and 25 vertices are within a brush radius of the tip against 65. This SHALL be verified at the TIP and SHALL NOT be verified by the surface-wide longest edge, which cannot see the rule: that maximum lives in the neck behind the tip either way and reads 0.1450 anchored against 0.1466 following, so a check written against it passes with the rule removed. The anchored arm also does 40% more topology work — 1381 splits against 980 — so what the rule buys is a refined tip and less work, not a smaller surface-wide maximum. (An earlier measurement of 1.12 against 0.36 was taken on an UNMAINTAINED carried region, where a handful of surviving vertices were dragged away alone; it is not what the maintained rule leaves.)

The maintenance SHALL be reported as counts — entries carried, entries inserted by a split, entries retired by a collapse, entries the remesher moved, and collapses refused — so that a maintenance which silently does nothing is visible as a count and not merely as a surface that looks about right. Collapses refused SHALL be reported as collapses AVOIDED and not as refusal events: the remesher asks about the same edge on each of its passes on each stamp, so the event count runs an order of magnitude above the operations it actually prevented.

#### Scenario: A carried region survives the remesher
- **WHEN** a long grab stroke drags an adaptive surface far enough that the remesher splits and collapses inside the carried region, with the split count asserted to be non-zero
- **THEN** the region still carried at the end of the gesture accounts for every entry captured, plus those a split inserted, minus those a collapse retired

#### Scenario: A maintained region keeps the weight that carries the drag
- **WHEN** the same stroke is run on a brush small enough against the detail resolution that an unmaintained region loses every entry it captured
- **THEN** the largest weight still carried at the last stamp is near the one captured at the first, rather than a region that is maintained in number while the vertices that carry the whole drag have been retired

#### Scenario: A carried vertex is not collapsed away
- **WHEN** a grab drags an adaptive surface with a brush small enough against the detail resolution that the remesher would otherwise retire the whole captured region
- **THEN** no entry of the carried region is retired for the length of the gesture, the largest weight carried at the last stamp is the one captured at the first, and the gesture reaches the whole drag on both representations

#### Scenario: Refusing those collapses does not coarsen the surface
- **WHEN** the same stroke is compared against the same stroke with the collapse refusal removed
- **THEN** the longest edge left on the surface is no longer with the refusal than without it, so the refusal is not the adaptive representation giving up the work it exists to do

#### Scenario: A vertex born of one carried parent joins the region
- **WHEN** the remesher splits an edge with exactly one endpoint in the carried region, partway through a grab
- **THEN** the new vertex joins the region carrying half its carried parent's weight, and ends the gesture where the midpoint of that parent's captured position and the other endpoint's position, plus its weighted share of the whole drag, puts it

#### Scenario: The remesher's own movement survives the next stamp
- **WHEN** the remesh relaxes a carried vertex along the surface between two stamps of a grab
- **THEN** the next stamp writes that vertex from its shifted captured position, so the relaxation is still visible at the end of the gesture rather than undone by the following write

#### Scenario: A vertex born mid-gesture is not dragged twice
- **WHEN** the remesher splits an edge whose endpoints are both carried, partway through a grab
- **THEN** the new vertex ends the gesture where its parents' captured midpoint plus its weighted share of the whole drag puts it, rather than a further drag beyond it

#### Scenario: The grab remesh follows the stamp
- **WHEN** a grab drags an adaptive surface several brush radii from where it started
- **THEN** the stretched region is remeshed as the gesture passes through it, rather than the remesh repeatedly refining the place the gesture began
