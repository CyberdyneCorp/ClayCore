# scene-model Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Document structure
`clay::scene` SHALL model a document as a list of layers, each `voxel` or `sdf` kind, with per-layer transform, visibility, resolution, and material. SDF layers SHALL hold an ordered edit list where each item applies to the combined result of all preceding items. Groups SHALL nest to depth ≥ 4 and carry group ops (including None). Layer instancing SHALL share content by reference such that editing the source updates all instances.

#### Scenario: Order matters
- **WHEN** an edit list [add sphere, subtract box] is reordered to [subtract box, add sphere]
- **THEN** evaluation produces a different field (subtract-before-add has nothing to remove), demonstrating ordered semantics

#### Scenario: Instance follows source
- **WHEN** a layer is instanced twice and an edit item is added to the source layer
- **THEN** both instances evaluate with the new item without duplicating stored content

### Requirement: Influence bounds
Every edit item and group SHALL expose a conservative influence bound: its shape AABB dilated by blend radius and rounding. The bound SHALL be conservative in the narrow-band sense that all evaluated storage relies on: outside the bound (dilated by the band width), band-clamped field values are unaffected by the item. (Raw far-field values may legitimately shift when a smooth-blend operand changes — smin deviates wherever |a−b| is inside the support width — which is why the guarantee, like brick storage, is stated band-clamped.)

When an item carries deformers, its bound SHALL additionally account for the domain warp before transform and dilation: rotational warps (twist, bend) SHALL widen the bound to the axis-aligned hull of the shape's rotational sweep, cross-section scaling (taper) SHALL scale by the largest factor in its range, and displacement SHALL dilate by its amplitude.

#### Scenario: Bound is conservative
- **WHEN** a property test samples the field with and without an item at points outside the item's influence bound dilated by a band width β, clamping values to ±β
- **THEN** the two clamped fields are bit-identical at every sampled point

#### Scenario: Deformed item stays inside its bound
- **WHEN** the same property test runs on items carrying twist, bend, taper, and displacement deformers
- **THEN** the clamped fields remain bit-identical outside the widened bound, and per-brick culled tapes over those scenes stay band-clamp identical to the full tape

### Requirement: Blend locality guarantee
Because all blends are rigid, an edit whose influence bound does not intersect a region SHALL leave evaluated data (bricks, samples) for that region bit-identical. This SHALL be regression-tested at the brick level.

#### Scenario: Distant edit leaves bricks untouched
- **WHEN** a scene with a filled brick cache receives a new edit whose influence bound intersects none of a set of bricks
- **THEN** re-evaluation of those bricks produces bit-identical brick data

### Requirement: Tape compilation
The scene module SHALL compile an edit list into a flat postfix tape: opcode stream plus parameter blocks with transforms pre-inverted. Scenes SHALL NOT be compiled into shader source; every backend runs a fixed tape-interpreter kernel, so parameter edits never trigger kernel recompilation.

#### Scenario: Parameter edit is recompile-free
- **WHEN** an item parameter (e.g. sphere radius) changes
- **THEN** only the tape's parameter block is rewritten; the backend kernel binary is unchanged and re-evaluation can start immediately

#### Scenario: Tape round-trip fidelity
- **WHEN** any scene in the golden corpus is compiled to a tape and evaluated on the CPU reference
- **THEN** results equal direct tree evaluation within 1e-6

### Requirement: Per-brick tape culling
For brick evaluation the compiler SHALL emit per-brick tapes containing only the items whose influence bound intersects that brick (the Dreams design), preserving evaluation semantics exactly.

#### Scenario: Culled tape matches full tape
- **WHEN** a brick is evaluated with its culled tape and with the full scene tape
- **THEN** the brick data is bit-identical, and the culled tape length is ≤ the full tape length

### Requirement: Undo command vocabulary
Every document mutation SHALL be expressed as a serializable command with a computable inverse: add/remove/reorder item, set parameter, voxel-span edit, layer add/remove/reorder/retransform, group/ungroup. The in-memory undo stack and the document file format SHALL share this single command vocabulary. Consecutive commands from one stroke SHALL be coalescable into a single undo step. Item state carried by commands SHALL include any deformer chain, so deformed documents round-trip.

The undo stack SHALL be reachable from the bindings, so a host application uses the engine's undo rather than reimplementing one over a second vocabulary that could disagree with what a saved document records.

#### Scenario: Command inverse restores state
- **WHEN** any command from the vocabulary is applied to a document and then its inverse is applied
- **THEN** the document state is bit-identical to the original (verified by serialization comparison)

#### Scenario: Stroke coalescing
- **WHEN** a sculpt stroke generates N incremental point-append commands followed by stroke end
- **THEN** undo removes the entire stroke as one step

#### Scenario: Deformed item round trip
- **WHEN** a document containing an item with a deformer chain is serialized and reloaded
- **THEN** the reloaded document evaluates bit-identically and re-serializes to identical bytes

#### Scenario: A host application undoes through the engine
- **WHEN** a binding performs an edit on a document with undo enabled and then undoes it
- **THEN** the document serializes bit-identically to its state before the edit

### Requirement: Non-local combine modes report infinite influence
A combine mode whose weight is non-zero arbitrarily far from both operands SHALL report an infinite influence bound, so per-brick culling never drops it. Transition morphs are such modes: the linear weight is non-zero over a half-space and the radial weight past a radius. This preserves the blend-locality guarantee by refusing to claim locality that does not exist, rather than by silently corrupting culled bricks.

#### Scenario: Transition item is never culled
- **WHEN** an item combined with a transition mode is compiled for a brick far from both operands
- **THEN** the item still appears in the culled tape, and the culled result is band-clamp identical to the full tape

#### Scenario: Locality is preserved for rigid blends alongside transitions
- **WHEN** a scene mixes transition items with ordinary smooth-blend items
- **THEN** the smooth-blend items are still culled where their influence bounds do not reach the brick

### Requirement: Influence bounds for lifted profiles
An item whose primitive is a lift SHALL compute its local bound from the profile: an extrusion bounds the profile's 2D extent across the extrusion depth, and a revolution sweeps the profile's radial extent into an annulus around the axis. Polygon profiles SHALL derive their extent from their vertices.

#### Scenario: Lifted item stays inside its bound
- **WHEN** the influence-bound property test runs on extruded and revolved items, including a concave polygon profile
- **THEN** band-clamped field values outside the bound are bit-identical with and without the item, and per-brick culled tapes stay band-clamp identical

#### Scenario: Revolved bound covers the full sweep
- **WHEN** a profile offset from the axis is revolved
- **THEN** the bound covers the whole circular sweep, not just the profile's own quadrant

### Requirement: Influence bounds for repetition
A repeated item's influence bound SHALL cover every copy it produces: a finite grid sweeps the item's bound across its occupied cell range, and a radial array sweeps it into an annulus about the axis — both finite and therefore cullable. An infinite grid SHALL report infinite influence, since it produces copies arbitrarily far away.

#### Scenario: Finite array stays inside its bound
- **WHEN** the influence-bound property test runs on finite grid and radial array items
- **THEN** band-clamped field values outside the bound are bit-identical with and without the item, and per-brick culled tapes stay band-clamp identical

#### Scenario: Infinite grid is never culled
- **WHEN** an item with an infinite grid repetition is compiled for any brick
- **THEN** it appears in the culled tape and the culled result is band-clamp identical to the full tape

### Requirement: A layer may carry a mask
A layer SHALL optionally carry a mask field, absent by default, stored beside its voxel content and keyed by layer id rather than inside the evaluated document. Its presence SHALL NOT change how the layer evaluates: masking gates where edits are authored, not where the field is sampled, so per-brick culling and blend rigidity are unaffected. Keeping the mask out of the evaluated document makes that structural rather than a property to be maintained.

#### Scenario: Evaluation is unchanged by a mask
- **WHEN** a mask is painted on a layer and the document is evaluated
- **THEN** the field is bit-identical to the same document without the mask

#### Scenario: Freeze protects what comes next
- **WHEN** a region is masked and a further edit is authored across it
- **THEN** the masked region is spared, while items already in the list are unaffected by the mask

### Requirement: A layer may be ghosted or locked
A layer SHALL carry a ghost flag and a lock flag, both off by default. A ghosted layer is still evaluated but is excluded from picking and from edits. A locked layer is still evaluated and still picked, but is excluded from edits. Neither flag SHALL change what a document evaluates to.

Both flags SHALL be settable through the command vocabulary, so that setting one is undoable and serializes with the document. A document written before the flags existed SHALL load with both off.

#### Scenario: Neither flag changes the field
- **WHEN** a layer is ghosted, or locked, and the document is evaluated
- **THEN** the field is bit-identical to the same document without the flag

#### Scenario: Setting a flag is undoable
- **WHEN** a layer is ghosted and the edit is undone
- **THEN** the layer is no longer ghosted, and the document matches what it was

#### Scenario: The flags round trip
- **WHEN** a document with a ghosted layer and a locked layer is saved and reloaded
- **THEN** both flags come back set

#### Scenario: An older document loads unprotected
- **WHEN** a document written before the flags existed is loaded
- **THEN** every layer is neither ghosted nor locked

### Requirement: Edits refuse protected layers
An edit naming a ghosted or locked layer SHALL be refused with a typed error and SHALL leave the document unchanged. It SHALL NOT be silently applied, and SHALL NOT be silently dropped: a host that greys the layer out wants the refusal, and one that does not must not quietly discard the artist's work.

Changing the flags themselves SHALL remain possible on a protected layer — otherwise locking would be irreversible.

#### Scenario: A locked layer refuses an edit
- **WHEN** an item is added to a locked layer
- **THEN** the edit is refused and the layer's edit list is unchanged

#### Scenario: A ghosted layer refuses an edit
- **WHEN** an existing node in a ghosted layer is retransformed
- **THEN** the edit is refused and the node is unchanged

#### Scenario: A protected layer can be unprotected
- **WHEN** a locked layer is unlocked and then edited
- **THEN** the unlock succeeds and the edit lands

### Requirement: Stroke points carry a type
A stroke point SHALL carry an interpolation type — hard corner, spline, B-spline, or Bezier — defaulting to hard corner. A Bezier point SHALL additionally carry an incoming and an outgoing handle, expressed in the item's local space relative to the point.

A point list SHALL be able to be marked closed, so that the last point connects back to the first.

#### Scenario: A hard point list is the stroke it always was
- **WHEN** a point list whose points are all hard corners is compiled
- **THEN** the tape is bit-identical to the one the same points produced before types existed

#### Scenario: Smooth points curve
- **WHEN** three points are given spline type and the item is evaluated
- **THEN** the surface passes outside the straight chain the same points would produce, and through every control point

#### Scenario: Bezier handles shape the span
- **WHEN** a Bezier point's handles are lengthened
- **THEN** the surface changes, and moving the handles back restores it

#### Scenario: A closed curve joins its ends
- **WHEN** a point list is marked closed
- **THEN** the span between the last point and the first is present in the field

### Requirement: Curves tessellate to a stated tolerance
A curve SHALL be tessellated into the segment chain the stroke opcode evaluates, subdividing a span while its midpoint deviates from its chord by more than the item's tolerance, to a bounded depth. The tolerance SHALL be a property of the document rather than of the host, so that two builds agree on what a document means.

Tessellation SHALL be deterministic: the same control points and tolerance SHALL produce the same segment chain, on every platform and through every binding.

#### Scenario: A tighter tolerance means a closer curve
- **WHEN** the same curve is compiled at a coarse and at a fine tolerance
- **THEN** the fine one uses more segments, and its surface lies closer to the ideal curve

#### Scenario: Tessellation is reproducible
- **WHEN** the same curve is compiled twice
- **THEN** the segment chains are identical

#### Scenario: Subdivision is bounded
- **WHEN** a curve is given a tolerance small enough to demand unbounded subdivision
- **THEN** subdivision stops at the bound rather than growing without limit

### Requirement: Editing a curve is an ordinary edit
Replacing an item's point list SHALL be expressed as a command, so that it is undoable, serializable and refused on a protected layer like every other edit. Its inverse SHALL restore the previous list exactly.

#### Scenario: Editing a curve is undoable
- **WHEN** a curve's points are replaced and the edit is undone
- **THEN** the document is exactly what it was

#### Scenario: A protected layer refuses a curve edit
- **WHEN** a curve on a locked layer has its points replaced
- **THEN** the edit is refused and the curve is unchanged

### Requirement: Curve bounds cover the tessellated curve
An item's bounds SHALL be computed from the tessellated points rather than from the control points, because a spline may pass outside the polygon its control points form. Picking and per-brick culling SHALL therefore not miss a curve that bulges beyond its control points.

#### Scenario: A bulging curve is still picked
- **WHEN** a ray is aimed at the part of a spline that lies outside its control-point hull
- **THEN** the ray reports a hit on that item

### Requirement: An item may carry a list of profiles
An item SHALL be able to carry two or more 2D profiles, each with its own polygon vertices where it is a polygon profile. The single-profile lifts SHALL keep the field they already use, so no existing document changes meaning.

A loft with fewer than two profiles SHALL be refused rather than compiled into a degenerate shape.

#### Scenario: A loft round trips
- **WHEN** a document containing a loft of a circle and a polygon is saved and reloaded
- **THEN** every profile, its parameters and its vertices come back, and the field is unchanged

#### Scenario: Existing lifts are unaffected
- **WHEN** a document containing an extrusion is compiled before and after this change
- **THEN** the tape is identical

#### Scenario: A degenerate loft is refused
- **WHEN** a loft is built with one profile or none
- **THEN** it is refused

### Requirement: A swept item carries a guide and profiles
A swept item SHALL carry a guide as control points with the same types, handles and tolerance a curve item uses, and SHALL carry its profiles in the same list a loft uses. A guide SHALL NOT be a new kind of curve.

#### Scenario: A sweep round trips
- **WHEN** a document containing a sweep with a spline guide and three profiles is saved and reloaded
- **THEN** the guide's control points and types, and every profile, come back, and the field is unchanged

#### Scenario: The guide honours its point types
- **WHEN** the same guide points are given hard and then spline types
- **THEN** the swept shapes differ

### Requirement: An item may carry a sampled volume
An item SHALL be able to carry a sampled volume as its primitive, shared between items by reference so that instancing one costs no extra storage. A volume SHALL survive a save and reload.

#### Scenario: A volume round trips
- **WHEN** a document containing a sampled volume is saved and reloaded
- **THEN** the field is unchanged

#### Scenario: An empty volume is refused
- **WHEN** an item carries a volume with no bricks and no samples
- **THEN** it contributes nothing rather than reading unwritten data

#### Scenario: A malformed volume fails the read
- **WHEN** a saved document's volume payload is truncated
- **THEN** the read fails rather than loading an item that would silently contribute nothing

#### Scenario: An older document still reads
- **WHEN** a document written before volumes existed is loaded
- **THEN** it loads without one, and the fields written after it in the record are unchanged

### Requirement: A node's deformer chain is editable through the command vocabulary
The command vocabulary SHALL be able to replace a node's deformer chain, as it can already replace that node's transform, primitive, colour, op and stroke points. The replacement SHALL be of the WHOLE list, and its inverse SHALL be the list that was there before.

Without it a deformer can only be set when a node is created, so no verb built on deformers can act on an existing sculpt — and any that tried would escape undo, which every other destructive operation is required not to do.

A whole-list replace is chosen over granular add and remove for the reason `SetStrokePointsCmd` was: a chain is a handful of records, so replacing it costs less than the commands to edit it would, and its inverse is exact by construction rather than by reconstruction.

#### Scenario: A chain is replaced and undone
- **WHEN** a node's deformers are replaced and the edit is undone
- **THEN** the node evaluates exactly as it did before the replacement

#### Scenario: The chain survives the document format
- **WHEN** a document whose node has a replaced deformer chain is saved and reloaded
- **THEN** it evaluates identically

#### Scenario: A missing node is refused
- **WHEN** the command names a node or layer that does not exist
- **THEN** it is refused rather than silently doing nothing

### Requirement: Deformer order is part of the contract
A node's deformers SHALL apply in authoring order, with `deformers[0]` warping the point first, so that the FIRST entry is the outermost warp on the resulting geometry and the last is the one nearest the primitive.

This is already what the evaluator does; stating it makes it something a caller may rely on. A verb that warps the assembled shape SHALL therefore insert its deformer at the FRONT of the chain, because one appended at the back has its region weight evaluated at a point the earlier deformers have already moved — and so acts somewhere other than where the caller aimed it.

#### Scenario: Position in the chain changes the result
- **WHEN** the same two deformers are applied to one item in both orders
- **THEN** the resulting fields differ

#### Scenario: A prepended warp acts where it was aimed
- **WHEN** a region warp is prepended to a chain whose existing deformer moves the region
- **THEN** the warp acts at the position the caller gave, not at the moved one

