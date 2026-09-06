# scene-model Specification

## Purpose
What a document IS and what an edit to one means: layers of either kind, the
ordered edit list an SDF layer holds, groups, instances, the per-layer transform
and the influence bounds that keep an edit LOCAL, the tape a document compiles
to, and the one undo history that reverses every representation through it.

The layer between the arithmetic below and everything that consumes it. A
document is the only thing a host, a file, a binding and a renderer all agree
about, so what it is has to be stated in one place rather than assumed in five.

## Requirements

### Requirement: Document structure
`clay::scene` SHALL model a document as a list of layers, each `voxel`, `sdf` or `mesh` kind, with per-layer transform, visibility, resolution, and material. SDF layers SHALL hold an ordered edit list where each item applies to the combined result of all preceding items. Groups SHALL nest to depth ≥ 4 and carry group ops (including None). Layer instancing SHALL share content by reference such that editing the source updates all instances.

A `mesh` layer carries imported geometry for display and re-export and is not evaluated; it is not an operand and SHALL NOT be instanced.

#### Scenario: Order matters
- **WHEN** an edit list [add sphere, subtract box] is reordered to [subtract box, add sphere]
- **THEN** evaluation produces a different field (subtract-before-add has nothing to remove), demonstrating ordered semantics

#### Scenario: Instance follows source
- **WHEN** a layer is instanced twice and an edit item is added to the source layer
- **THEN** both instances evaluate with the new item without duplicating stored content

#### Scenario: A mesh layer is not an operand
- **WHEN** a mesh layer is instanced
- **THEN** the call is refused, as it already is for a voxel layer

### Requirement: Influence bounds
Every edit item and group SHALL expose a conservative influence bound: its shape AABB dilated by blend radius and rounding. The bound SHALL be conservative in the narrow-band sense that all evaluated storage relies on: outside the bound (dilated by the band width), band-clamped field values are unaffected by the item. (Raw far-field values may legitimately shift when a smooth-blend operand changes — smin deviates wherever |a−b| is inside the support width — which is why the guarantee, like brick storage, is stated band-clamped.)

When an item carries deformers, its bound SHALL additionally account for the domain warp before transform and dilation: rotational warps (twist, bend) SHALL widen the bound to the axis-aligned hull of the shape's rotational sweep, cross-section scaling (taper) SHALL scale by the largest factor in its range, and displacement SHALL dilate by its amplitude.

A node held inside one or more groups SHALL additionally expose the bound it reaches THROUGH those groups: its own bound dilated, at each enclosing group in turn, by that group's blend support. This is the conservative answer in the same band-clamped sense as the bound above — outside it, band-clamped values are unaffected by an edit to that node — and it SHALL be the answer given wherever a caller asks where an edit to that node lands. A group's ancestry SHALL contribute its blend support and nothing else: the group's other children are geometry the edit cannot reach, and SHALL NOT widen the answer.

Where the enclosing subtree combines non-locally, the ancestor walk SHALL report the unbounded state rather than a finite box, on the same terms as the influence bound of any non-local node.

#### Scenario: Bound is conservative
- **WHEN** a property test samples the field with and without an item at points outside the item's influence bound dilated by a band width β, clamping values to ±β
- **THEN** the two clamped fields are bit-identical at every sampled point

#### Scenario: Deformed item stays inside its bound
- **WHEN** the same property test runs on items carrying twist, bend, taper, and displacement deformers
- **THEN** the clamped fields remain bit-identical outside the widened bound, and per-brick culled tapes over those scenes stay band-clamp identical to the full tape

#### Scenario: A node inside a group reaches past its own box
- **WHEN** the conservativeness property test runs on a child of a smooth-blended group, sampling outside the child's own bound but inside the group's blend support
- **THEN** the band-clamped fields differ there, so the child's own bound alone is NOT the answer to where an edit to it lands

#### Scenario: The ancestor-path bound is conservative
- **WHEN** the same property test samples outside the child's bound dilated by every enclosing group's blend support
- **THEN** the two band-clamped fields are bit-identical at every sampled point

#### Scenario: A far sibling is not part of the answer
- **GIVEN** a group holding one small child and a large one far from it
- **WHEN** the ancestor-path bound of the small child is taken
- **THEN** it is strictly smaller than the group's influence bound and does not contain the far sibling's geometry

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

Deciding which items those are SHALL NOT require visiting every item in the document. The compiler SHALL consult a spatial index over item influence bounds, so that the cost of culling one brick scales with the number of items NEAR that brick rather than with the size of the document.

The index SHALL be derived from the same definition of reach the compiler already uses — `item_influence_bound`, and `item_influence_is_local` for whether an item has a bound at all — so that no second notion of what an item touches can go stale against the first. An item that is not local SHALL be emitted unconditionally rather than placed in the index.

The index SHALL be owned by, and invalidated with, the compiled tape it culls for, so that a document mutation cannot leave the index and the tape disagreeing about the same document.

#### Scenario: Culled tape matches full tape
- **WHEN** a brick is evaluated with its culled tape and with the full scene tape
- **THEN** the brick data is bit-identical, and the culled tape length is ≤ the full tape length

#### Scenario: Culling cost does not follow document size
- **WHEN** the same brick is culled from a document of 100 items and from a document of 10 000 items with the same local density
- **THEN** the time to produce the culled tape does not grow in proportion to the item count

#### Scenario: A non-local item is never culled away
- **WHEN** a document contains an item whose influence is unbounded and a brick that its geometry does not come near
- **THEN** that item is present in the brick's culled tape, and the brick data is bit-identical to the full-tape result

#### Scenario: An edit is visible to the next cull
- **WHEN** an item is added, moved or removed and a brick is culled immediately afterwards
- **THEN** the culled tape reflects the edit, exactly as a full recompile would

### Requirement: Undo command vocabulary
Every document mutation SHALL be expressed as a serializable command with a computable inverse: add/remove/reorder item, set parameter, voxel-span edit, layer add/remove/reorder/retransform. Grouping is not a command of its own: a group is a node, so creating one is an add, deleting one is a remove that carries the whole subtree back, and grouping N existing siblings is N reparents bracketed into a single undo step. The in-memory undo stack and the document file format SHALL share this single command vocabulary. Consecutive commands from one stroke SHALL be coalescable into a single undo step. Item state carried by commands SHALL include any deformer chain, so deformed documents round-trip.

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

#### Scenario: An edit to a group undoes exactly
- **WHEN** a group's op is changed, a child is added to it, it is reparented, or the whole group is removed, on a document with undo enabled
- **THEN** one undo restores the document to bit-identical bytes

### Requirement: Non-local combine modes report infinite influence
A combine mode whose weight is non-zero arbitrarily far from both operands SHALL report an infinite influence bound, so per-brick culling never drops it. Transition morphs are such modes: the linear weight is non-zero over a half-space and the radial weight past a radius. This preserves the blend-locality guarantee by refusing to claim locality that does not exist, rather than by silently corrupting culled bricks.

An INTERSECT is not such a mode and SHALL NOT report an infinite influence bound. `max(acc, item)` can only take material away, and what it takes away is inside what the layer already occupies, so an intersect's influence SHALL be the LAYER's own extent — the union of its visible items' geometry bounds — and infinite only when that union is itself unbounded. An intersect whose non-locality has a second cause, such as an infinite grid repeat or an unbounded primitive, SHALL remain infinite: the weaker answer does not win.

CULLABILITY IS A SEPARATE QUESTION and SHALL NOT change. Every non-local op, intersect included, SHALL remain ineligible for per-brick culling, so an intersect still appears in every brick's tape. The finite bound answers only which bricks an edit dirties.

#### Scenario: Transition item is never culled
- **WHEN** an item combined with a transition mode is compiled for a brick far from both operands
- **THEN** the item still appears in the culled tape, and the culled result is band-clamp identical to the full tape

#### Scenario: Locality is preserved for rigid blends alongside transitions
- **WHEN** a scene mixes transition items with ordinary smooth-blend items
- **THEN** the smooth-blend items are still culled where their influence bounds do not reach the brick

#### Scenario: An intersect reports its layer's extent
- **WHEN** an item combined with intersect is queried for its influence bound in a layer whose other items reach further than it does
- **THEN** the bound is finite and contains those other items, and the item is still reported as ineligible for culling

#### Scenario: An intersect is still never culled
- **WHEN** an item combined with intersect is compiled for a brick its own geometry does not reach
- **THEN** the item still appears in the culled tape, and the culled result is band-clamp identical to the full tape

#### Scenario: An unbounded layer bounds nothing
- **WHEN** an intersect sits in a layer that also holds a primitive with no finite extent
- **THEN** its influence bound is infinite

#### Scenario: A second cause of non-locality wins
- **WHEN** an item combines with intersect AND repeats on an infinite grid
- **THEN** its influence bound is infinite

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

### Requirement: The layer mirror applies to every item by default
A layer SHALL carry mirror axes (any of x/y/z, off by default) and a Mirror Blend seam width. When an axis is enabled, evaluation SHALL reflect each participating item through the plane where that layer-local coordinate is 0, combined with the item under the layer's Mirror Blend — a hard crease at width 0, a smooth weld above it.

Every item SHALL participate by default, placed items and stroke stamps alike: turning symmetry on means the layer, so an item opts OUT (`Node::mirror = false`; `-1` across the C ABI, `mirror=False` in Python) rather than in. The mirror is a property the evaluation reads, not an edit baked into items — enabling it before or after the items were added SHALL evaluate identically, and disabling it SHALL restore the unmirrored field. A layer with no mirror axes SHALL evaluate identically whatever its items' participation flags, at no added cost.

Loading a document SHALL preserve each stored node's participation flag, so a document saved under the opt-in default (through 0.27.3) evaluates as it did when it was saved.

#### Scenario: Setting the layer mirror mirrors a placed item
- **WHEN** a mirror about x is set on a layer holding a unit sphere, and an off-centre lump is added with a zeroed descriptor
- **THEN** raycasts along the lump's direction and its mirror image both report the lump, through the document raycast and the brick cache alike

#### Scenario: The mirror applies regardless of edit order
- **WHEN** the same mirror is set after the items were added instead of before
- **THEN** the document evaluates identically

#### Scenario: An item opts out
- **WHEN** the lump is added with mirror participation −1
- **THEN** the near side carries the lump and the mirrored side reads the untouched sphere

#### Scenario: A stroke lands on both sides
- **WHEN** a relief stroke is applied to a mirrored layer
- **THEN** the mirrored side carries the same bulge as the stroked side, not spillover

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
Replacing an item's point list SHALL be expressed as a command, so that it is undoable, serializable and refused on a protected layer like every other edit. Its inverse SHALL restore the previous list exactly. The command SHALL apply to a swept item's guide as well as to a stroke, since a guide is the same control-point list and not a new kind of curve; a node that carries no such list SHALL still be refused.

#### Scenario: Editing a curve is undoable
- **WHEN** a curve's points are replaced and the edit is undone
- **THEN** the document is exactly what it was

#### Scenario: A protected layer refuses a curve edit
- **WHEN** a curve on a locked layer has its points replaced
- **THEN** the edit is refused and the curve is unchanged

#### Scenario: A placed sweep's guide is editable
- **WHEN** a swept item's points are replaced with a differently shaped guide
- **THEN** the edit applies, and its inverse restores the guide that was there

#### Scenario: A node with no point list is refused
- **WHEN** the replace names a primitive that carries no control points
- **THEN** it fails and the document is untouched

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

### Requirement: An item's bound is derived once per compile
Compiling a document SHALL compute each item's world bound once. The influence bound of a local item IS its geometry bound, so asking for both means doing the same work twice — and for a stroke or a sweep that work re-tessellates the curve.

#### Scenario: A stroke-heavy document compiles without re-tessellating per item
- **WHEN** a document of curve-based items is compiled
- **THEN** each item's bound is derived once

#### Scenario: The tape is unchanged
- **WHEN** any document is compiled
- **THEN** the resulting tape's instructions, parameters, blob and bounds are exactly what they were before

### Requirement: Whether an item may be culled has a single definition
The test for whether an item's influence is confined to its geometry SHALL exist in exactly one place, and the influence bound SHALL be defined in terms of it. A caller holding the geometry bound already SHALL be able to ask the question without recomputing the bound.

This matters beyond tidiness. A second copy of the test that fell out of step would declare a non-local item cullable, and per-brick tapes would drop it while the whole-document tape kept it — so the field would be wrong only inside bricks that do not touch the item, and no whole-document assertion would notice.

#### Scenario: The predicate and the bound agree
- **WHEN** an item carries a non-local op, an infinite grid repeat, or a primitive with no finite extent
- **THEN** the predicate reports it as not local AND its influence bound is infinite

#### Scenario: An ordinary item is local and finite
- **WHEN** an item carries a local op, no infinite repeat and a bounded primitive
- **THEN** the predicate reports it as local AND its influence bound is finite

#### Scenario: A non-local item survives a distant cull region
- **WHEN** a document is compiled against a cull region far from every item's geometry
- **THEN** items whose influence is not local are still emitted, and only the local ones are dropped

#### Scenario: A cull region covering everything changes nothing
- **WHEN** a document is compiled against a cull region containing all of it, and again with no cull region
- **THEN** the two tapes are identical

### Requirement: An armature is edited as a tree
The module SHALL provide edits that add a child to a node, move a node, set a node's radius, set a node's sign, and delete a node together with its subtree. Each SHALL go through the command vocabulary, so it is undoable, refused on a protected layer, and serialised with the document.

Moving a node SHALL move its subtree with it. This is the property the feature exists for: an arm hangs from a shoulder, so moving the shoulder carries the arm rather than leaving it behind.

A node added as a child SHALL be positive; the sign edit flips it. Deleting a node SHALL take its signs with its subtree, so no node is left with another node's sign, and a negative node SHALL NOT be required to be a leaf.

#### Scenario: Moving a parent carries its subtree
- **WHEN** a node with descendants is moved
- **THEN** every descendant moves by the same displacement, and their positions relative to it are unchanged

#### Scenario: Deleting a node takes its subtree
- **WHEN** a node with descendants is deleted
- **THEN** the descendants go with it, and no node is left naming a parent that no longer exists

#### Scenario: Every armature edit undoes exactly
- **WHEN** any armature edit is applied and undone
- **THEN** the document matches what it was before, including the tree's shape, every radius and every sign

#### Scenario: Deleting a subtree keeps the survivors' signs
- **WHEN** a subtree containing a negative node is deleted from a rig whose other nodes mix signs
- **THEN** every surviving node keeps its own sign under the renumbering

### Requirement: An armature can be authored symmetrically
Adding a node with mirroring SHALL add the node and its reflection through the layer's mirror as ONE undo step, following the precedent the voxel mirrored write already sets.

This is an authoring rule rather than a field one: the layer mirror already reflects what is evaluated, and what is missing is that building one arm builds the other.

#### Scenario: A mirrored insert is one step
- **WHEN** a child is added with mirroring on, and the edit is undone
- **THEN** both the node and its reflection are gone, in one undo

### Requirement: An armature persists with the document
A document containing an armature SHALL save and reload evaluating bit-identically, carrying the tree's shape and signs as well as its positions and radii.

The format SHALL stay backward-open: a reader that does not know armatures SHALL skip one rather than refusing the document.

#### Scenario: An armature round trips
- **WHEN** a document containing a branching armature is saved and reloaded
- **THEN** it evaluates bit-identically and reserialises to identical bytes

#### Scenario: A negative node survives the round trip
- **WHEN** a document containing an armature with a negative node is saved and reloaded
- **THEN** the sign is still there, the hollow still evaluates, and the node can be flipped positive again

### Requirement: Consolidation preserves the colours it bakes
Consolidating a layer SHALL write the per-item colours into the resulting volume's colour channel. Consolidation is advertised as changing what a layer COSTS rather than what it looks like, and collapsing every colour in a layer to the one on the resulting node contradicts that: a consolidated character currently loses the distinction between skin and armour.

The volume's colour SHALL take precedence over the node's where a sample carries one, and the node's colour SHALL remain the answer outside the sampled box and for a volume with no colour.

`Op::Paint` SHALL continue to override both. It is the operator whose whole purpose is to set colour, and a volume that ignored it would make painting over a consolidated layer impossible.

Consolidation SHALL NOT fill a colour channel when the absorbed set cannot produce more than one colour. Filling it is a second evaluation of the tape at every surviving sample, and where every absorbed node carries the same colour the result is that one colour repeated — which the node's own colour already reports, by the rule above. The absorbed set can produce more than one colour when two or more distinct node colours appear in it, or when any absorbed node is a volume whose samples carry colour of their own; a volume with a colour channel has one node colour and many sample colours, so a test on node colours alone SHALL NOT be the whole condition.

The decision SHALL be made from the absorbed set rather than from the samples, so that a layer which does not need the pass never pays it.

#### Scenario: A two-colour layer consolidates to a two-colour volume
- **GIVEN** a layer holding a red item and a blue item
- **WHEN** the layer is consolidated and the result is sampled inside each item
- **THEN** the reported colours are red and blue, not one colour for both

#### Scenario: A one-colour layer consolidates without a colour channel
- **GIVEN** a layer whose items all carry the same colour
- **WHEN** the layer is consolidated
- **THEN** the resulting volume has no colour channel, the resulting node carries that colour, and sampling anywhere reports it

#### Scenario: A coloured volume is re-consolidated
- **GIVEN** a layer holding one item, itself a volume whose samples carry two colours
- **WHEN** the layer is consolidated again
- **THEN** the colour pass is taken and both colours survive, even though the absorbed set holds a single node colour

#### Scenario: Painting over a consolidated volume still works
- **WHEN** a Paint operation is applied over a consolidated coloured volume
- **THEN** the painted colour is reported, overriding the volume's own

### Requirement: Consolidation is one undoable command
Collapsing a region of an edit list into a volume SHALL be a single command, refused on a protected layer, whose inverse restores the items it absorbed — which means the undo record carries them rather than only the resulting volume.

The scope of a consolidation SHALL be a LAYER. An arbitrary run of siblings has
no well-defined field of its own: an edit list is ordered and its operators are
relative, so a Subtract in the middle of a list means nothing without what
precedes it. A layer does have one, because layers combine by hard union at the
document level — so baking a layer is exact with respect to the whole
document's result.

Consolidation SHALL sample the layer in its OWN frame, so the layer's transform
still applies to the result and consolidating moves nothing.

Items that are hidden SHALL NOT be absorbed. They contribute nothing to the
field, so absorbing them would spend their parameters on nothing.

#### Scenario: Consolidation undoes to the parametric form
- **WHEN** a region is consolidated and the command undone
- **THEN** the original items are present and editable by their parameters again

#### Scenario: One step, however many items
- **WHEN** a layer holding several items is consolidated
- **THEN** the undo stack grows by exactly one step

#### Scenario: A protected layer is refused before it is resampled
- **WHEN** consolidation names a ghosted or locked layer
- **THEN** it is refused and the document is unchanged, without the bake being performed

### Requirement: What a consolidated region still promises
The module SHALL state what survives consolidation and what does not: the surface within the baked resolution survives, and the parameters of the absorbed items do not — nor do their individual colours, since a volume carries one.

A host SHALL be able to tell which regions of a document are consolidated, so it can stop offering parameter edits there rather than failing them.

That answer SHALL come from the CONTENT rather than from a stored provenance
flag: a region is consolidated when its edit list is a single item carrying
samples. The promise a host has to make is about what the region IS — samples
at a fixed resolution, with no parameters to offer — and a mesh imported as a
volume is exactly as unparametric as a bake, so a flag distinguishing them
would split two cases a host must treat alike. It would also have to be
serialised to survive a save, which this change does not need.

Consolidation SHALL be one-way. What was absorbed is in the undo record, which
is where going back belongs; re-expansion would have to invent parameters for a
shape that no longer has any.

#### Scenario: A host can see what is baked
- **WHEN** a host asks about a consolidated region
- **THEN** it is told the region is baked and at what resolution

#### Scenario: A layer with a volume among other items is not consolidated
- **WHEN** a host asks about a layer holding a volume alongside parametric items
- **THEN** it is told the layer is not consolidated, because those items still have parameters to offer

### Requirement: A document that never consolidates is unchanged
Reporting a chain's degradation, quoting what consolidating would cost, and asking whether a region is consolidated SHALL all be reads: none of them SHALL change what a document serialises to or what it evaluates to.

#### Scenario: Asking costs nothing
- **WHEN** a host reports, quotes and inspects without consolidating
- **THEN** the document serialises to identical bytes and compiles to an identical tape

### Requirement: A gated item is a document concept that survives the file
An item's mask SHALL be part of the document: it SHALL serialize, reload and evaluate identically, and a document with no gated item SHALL serialize to exactly the bytes it does today.

#### Scenario: A document with no gated item is unchanged
- **WHEN** a document containing no masked item is serialized
- **THEN** the bytes are identical to those the previous format version produced

### Requirement: Gating does not widen an item's influence
A mask SHALL only reduce where an item acts. An item's influence bound SHALL therefore remain valid when the item is gated, so per-brick culling needs no change.

#### Scenario: A gated item culls exactly as its ungated form does
- **WHEN** the cull plan is built for a document whose item is gated
- **THEN** the bricks selected are the same as for the ungated item, and none that the gated item affects are skipped

### Requirement: The undo history is measurable and bounded
The undo history SHALL report what it costs and SHALL accept a memory budget.

A host SHALL be able to read, in one call, the bytes held by the undo and redo stacks and the depth of each. Bytes SHALL account for what the entries OWN — a recorded node's deformer chain and stroke points included — and not only the size of the command variant, because the entries that matter are the ones holding heap payloads.

A host SHALL be able to set a byte budget on the history. When the budget is exceeded the OLDEST undo entries SHALL be dropped until it is met. Dropping SHALL be from the far end only: the most recent step SHALL always be undoable while the history is non-empty, so a budget can never make the next undo fail.

A host SHALL be able to trim the history on demand without setting a budget, for a platform that reports memory pressure and expects an immediate response.

Truncation SHALL be observable and SHALL NOT be an error. A host SHALL be able to tell that the history no longer reaches as far back as it did, so it can present the horizon rather than let a user search for a step that is gone.

An unset budget SHALL mean unbounded, so a host that never sets one behaves exactly as before.

#### Scenario: The history reports what it holds
- **WHEN** a document with undo enabled receives a sequence of edits
- **THEN** the reported byte count grows, and an edit whose inverse carries a whole node reports more than one whose inverse carries an id

#### Scenario: A budget evicts the oldest step
- **WHEN** a budget is set below what the history currently holds
- **THEN** entries are dropped from the oldest end until the budget is met, the reported depth falls, and undo still reverses the most recent edit exactly

#### Scenario: The newest step survives any budget
- **WHEN** a budget smaller than a single entry is set and one edit is then performed
- **THEN** that edit is still undoable

#### Scenario: Trimming is not an error
- **WHEN** a host trims the history and then queries it
- **THEN** the call succeeds, the depth reflects the trim, and the host can distinguish a trimmed history from an empty one

#### Scenario: No budget means no change
- **WHEN** no budget is set
- **THEN** the history grows without eviction and every recorded step remains undoable

### Requirement: A layer carries a radial symmetry mode
A layer SHALL carry a radial symmetry described by a count, an axis and a seam blend. A count of 0 or 1 SHALL mean the mode is off and SHALL cost nothing at evaluation.

When the count is 2 or more, every participating item in the layer SHALL evaluate as itself plus `count - 1` copies, each rotated about the layer-local axis by `2πk / count` for `k` in `1 .. count-1`. The copies SHALL be combined into the item's own value, so a radial layer presents one accumulated field rather than N independent items.

The axis SHALL pass through the origin of the layer's local frame, so the layer transform moves it and it persists in the document. Clearing the count SHALL restore the un-arrayed field exactly.

#### Scenario: A single item becomes an N-fold rosette
- **WHEN** a layer with one off-axis sphere is given a radial count of 6 about Y
- **THEN** the field is invariant under rotation by 60° about the layer's local Y axis, and sampling at the sphere's centre rotated by any multiple of 60° returns the same distance

#### Scenario: Turning it off restores the field
- **WHEN** a radial count is set and then cleared
- **THEN** the document evaluates identically to a probe of the same points taken before the count was set

#### Scenario: The axis follows the layer
- **WHEN** the layer holding a radial array is translated
- **THEN** the array's centre moves with it, because the axis is the layer-local one rather than a world axis

### Requirement: Radial symmetry uses the mirror's participation rule
An item SHALL participate in its layer's radial symmetry under the same flag that governs its participation in the layer mirror. An item excluded from the layer mirror SHALL also be excluded from the radial array, so a single asymmetric detail is excluded once rather than twice.

A stroke SHALL participate, because a stroke resolves into ordinary items — which is the property that makes this a sculpting mode rather than a modelling one.

#### Scenario: An excluded item does not repeat
- **WHEN** an item on a radial layer has its mirror participation cleared
- **THEN** that item appears once while every other item in the layer appears `count` times

#### Scenario: A stroke on a radial layer repeats
- **WHEN** a stroke is applied to a layer with a radial count of 4
- **THEN** the stamps it resolved into each appear 4 times, without the caller touching the resolved nodes

### Requirement: The radial seam blends like the mirror seam
The radial seam blend SHALL follow the semantics of the mirror blend: 0 SHALL be a hard union between a copy and its neighbours, and a positive value SHALL smooth-weld them where they meet. A positive seam SHALL mark the layer's tape as smooth-blended for exactness tracking, exactly as the mirror seam does.

#### Scenario: A positive seam welds neighbouring copies
- **WHEN** two adjacent copies of an item overlap and the seam blend is positive
- **THEN** the surface between them is welded rather than creased, and the layer reports a smooth blend rather than an exact field

### Requirement: Radial and mirror compose additively
When both the radial count and one or more mirror axes are active, each SHALL contribute its own copies of the base item. The change SHALL NOT emit the products of the two — a rotated reflection is not emitted — which matches the existing mirror, where enabling two axes emits one reflection per axis rather than the four of a full two-plane symmetry.

#### Scenario: Both modes active emits both sets
- **WHEN** a layer has a radial count of 3 and mirroring on X
- **THEN** each participating item evaluates as itself, plus 2 rotated copies, plus 1 reflected copy, and not as the 6 of a combined group

### Requirement: A radial layer reports the influence it actually occupies
An item's influence bound on a radial layer SHALL cover every copy the mode emits, so culling and the brick cache do not drop a copy that is on screen.

#### Scenario: A culled region still sees the far copies
- **WHEN** a brick overlapping only the far side of a radial array is evaluated
- **THEN** the item whose copy reaches that brick is compiled into the brick's tape

### Requirement: A surface region can be named
The library SHALL provide a SURFACE GROUP: an identifier attachable to a region of a layer's surface, independent of how that layer stores its surface.

A surface point SHALL be resolvable to the group it belongs to, and a group SHALL be resolvable to the region it covers, on every representation the library holds. Where a representation cannot store a per-element id — an SDF layer has no elements — the mechanism SHALL be stated in the specification rather than left to the binding, so a host learns one concept and not three.

Group membership SHALL survive the operations that preserve a layer's identity: saving and loading, hiding and showing, transforming the layer, and reordering the stack. It SHALL NOT be claimed to survive a representation bridge, which resamples the surface; what happens across a bridge SHALL be stated explicitly and MAY be "the ids are gone".

A group SHALL support the set operations an artist expects of a selection: grow, shrink, and the border between a group and its complement. These SHALL be defined on the region rather than on the storage, so growing a group on a mesh and on a voxel grid mean the same thing.

#### Scenario: A point resolves to its group
- **WHEN** a region of a layer's surface is assigned a group and a point inside that region is queried
- **THEN** the query returns that group, and a point outside it does not

#### Scenario: Groups survive a save
- **WHEN** a document carrying surface groups is saved and reloaded
- **THEN** every group covers the same region it covered before

#### Scenario: Growing a group is defined on the region
- **WHEN** a group is grown by one step on two layers holding the same shape in different representations
- **THEN** both cover the geometrically corresponding region, within the coarser representation's resolution

### Requirement: Visibility applies to a region, not only to a layer
A host SHALL be able to hide part of a layer's surface, show it again, and invert what is hidden, addressing the part by surface group or by mask.

Hiding SHALL NOT delete. Hidden geometry SHALL persist through save and load and SHALL be restored exactly when shown, matching the guarantee a hidden LAYER already carries.

Hidden geometry SHALL be excluded from the operations that act on visible surface — evaluation for display, meshing, and picking — and a host SHALL be able to determine whether an operation respected the hidden set. **An operation that ignores hidden geometry SHALL say so**, because a brush that silently reaches hidden surface is worse than one that refuses.

#### Scenario: A hidden region contributes nothing and is not lost
- **WHEN** part of a layer is hidden, the document is saved and reloaded, and the region is shown again
- **THEN** the meshed surface omits the region while hidden, still omits it after the reload, and matches the original once shown

#### Scenario: Isolating is hiding the complement
- **WHEN** a group is isolated
- **THEN** the result is identical to hiding everything not in that group

#### Scenario: Hiding is undoable
- **WHEN** a region is hidden and the edit is undone
- **THEN** the visible surface is restored exactly

### Requirement: An appended document reuses its compiled prefix
When a document differs from one already compiled only by nodes appended to the end of the last visible SDF layer's root list, the compiler SHALL be able to produce the new tape by reusing the compiled prefix and compiling only the appended nodes.

The result SHALL be **bit-identical** to a full compile of the same document — every instruction, every parameter, every blob float, the field info and the bounds. Not "within tolerance": a tape that differs anywhere is a different field, and the reuse is only sound because emission is append-ordered and nothing already emitted moves.

Reuse SHALL be attempted only where that append-ordering holds. A node inserted anywhere but the tail, a node removed or moved, any edit to an already-compiled node, and an append to any layer but the last visible SDF layer all SHALL compile in full. Refusing to reuse costs a recompile, which is the behaviour that existed before; reusing when the prefix has in fact moved is silent and wrong, so where it is not obvious the compiler SHALL compile in full.

A compiled tape SHALL remain immutable, and its content identity SHALL keep meaning what it means today: a tape built from a reused prefix has different bytes from the tape whose prefix it borrowed, and therefore SHALL carry its own distinct identity rather than inheriting one.

#### Scenario: A reused prefix gives the same tape as a full compile
- **WHEN** a document is compiled, one or more nodes are appended to the end of the last visible SDF layer, and the tape is compiled again with prefix reuse
- **THEN** the resulting tape is bit-identical to compiling the appended document from scratch

#### Scenario: Reuse holds across what the compiler emits per item
- **WHEN** the appended-to layer carries a mirror, a radial symmetry, a mask or a layer transform, and when the appended nodes carry blends, groups, strokes, sampled volumes, gates or deformer chains
- **THEN** the reused result stays bit-identical to a full compile in every case

#### Scenario: An edit that is not a tail append compiles in full
- **WHEN** a node is inserted before the end of the list, removed, moved, or edited in place, or a node is appended to a layer that is not the last visible SDF layer
- **THEN** the tape is compiled in full and matches a fresh compile of the edited document

#### Scenario: A reused tape carries its own identity
- **WHEN** a tape is produced by reusing another tape's prefix
- **THEN** its content identity is nonzero and differs from that of the tape whose prefix it reused, and the tape it reused from is unchanged

### Requirement: Prefix reuse reports where the agreement ends
When a tape is compiled by reusing another's prefix, the compiler SHALL report the point up to which the two tapes agree, so that a consumer other than the compiler can act on it.

Without this the reuse is invisible outside the compiler: the resulting tape has different bytes overall and nothing downstream can tell that most of them are the ones it already had. Reporting the boundary is what turns a cheaper compile into a cheaper upload.

The reported point SHALL be the compiler's actual agreement point, not an estimate — a boundary claimed further along than the tapes agree is a field that never existed, and it fails silently.

#### Scenario: The agreement point is where the prefix ended
- **WHEN** a document is compiled by reusing a prefix
- **THEN** the reported offsets are the lengths of the reused prefix, and the two tapes' sections are byte-identical below them

#### Scenario: A full compile reports no agreement
- **WHEN** a document is compiled without reusing a prefix
- **THEN** no agreement point is reported

### Requirement: An appended item extends the cull index
The cull index SHALL be extendable for a document that has gained items at the tail of its last visible SDF layer's root list, without recomputing the bounds of the items already in it. A stroke appends one item per stamp and every stamp invalidates the index, so rebuilding it walks the whole document to add one item: 2.45 ms at 50,000 items, of which 2.29 ms is bounds that did not move.

An extension SHALL produce the index a fresh build produces — the same chains with the same cached bounds and the same prunability, and the same cull pad — so that the per-brick tapes compiled through it are byte-identical to those compiled through a rebuilt one. Chain ORDER need not match, because a plan keys chains by layer and child list and those keys are unique.

An extension SHALL reach EVERY chain over that root list, not only the last layer's. An instanced layer shares its content with the layer it instances, so one root list is compiled once per layer that names it, and each is its own chain with its own bounds because an item's geometry bound reads the layer's transform and mirror. Extending one and not the others leaves them describing a document that no longer exists, which is a silently smaller tape rather than a failure.

An extension SHALL be REFUSED, leaving the index unchanged, wherever it cannot be certain the document changed only in that way. A refusal costs the rebuild the caller would have paid anyway; a wrong extension is silent.

The cull pad SHALL be raised from the appended subtree rather than recomputed from the document, and SHALL be kept as its two terms PER LAYER. The document's pad is a maximum over layers of the sum of that layer's two maxima; folding the terms across layers instead would store a sum of maxima, which is larger — so safe to cull with — but no longer the number a fresh build reports. Per layer the two are the same number, which is what makes raising them exact.

The subtree an extension reads SHALL be every node it reaches, including the children of a group the build does not descend into, because the pad folds over the layer's flat node map and an invisible group's visible child sets it either way.

An extension SHALL be REFUSED when the touched layer's node map did not grow by exactly the subtree named. The pad's terms are raised from that subtree alone, so a map that gained anything else would leave them below what a fresh build reports, and a pad that is too small plans against too small a region — the one direction that loses items a brick needed.

#### Scenario: An extended index is a rebuilt index
- **GIVEN** a document whose last items were appended to a chain
- **WHEN** per-brick tapes are compiled through an index extended by that append and through one built fresh
- **THEN** the tapes are byte-identical, and the two indexes report the same cull pad

#### Scenario: An instanced layer's second chain is extended too
- **WHEN** an item is appended to a root list that two layers compile
- **THEN** both layers' chains carry it, each with the bound its own layer's transform gives

#### Scenario: A stroke's cost does not follow the document
- **WHEN** the same dab is appended to indexes over documents of very different sizes
- **THEN** what the extension costs is set by the appended item rather than by how much precedes it

#### Scenario: The pad an append raises is a maximum of sums
- **GIVEN** one layer whose pad is all feather and another whose pad is all blend
- **WHEN** an item is appended to the second
- **THEN** the document's pad is the larger of the two layers' sums, not the sum of the larger of each term

#### Scenario: A widening blend inside an appended group raises the pad
- **WHEN** the appended node is a group, or an invisible group, whose child blends wider than anything already in the layer
- **THEN** the extended index reports the pad a fresh build reports

#### Scenario: An uncertain append is refused
- **WHEN** the items claimed as appended are not the tail of the layer they name, or there are more of them than it holds
- **THEN** the index is left exactly as it was and the caller rebuilds

#### Scenario: An append the node map does not corroborate is refused
- **WHEN** the layer's node map gained a node outside the subtree the append names
- **THEN** the extension is refused rather than left with a pad below the one a rebuild would report

### Requirement: A hard blend drags no chain, and so pads no cull region
The pad a per-brick cull region takes for a smooth-union chain SHALL be the largest single-item DRAG in the layer, and a HARD profile drags nothing: its smin is a step, so the running value it produces is a plain minimum and moves by no blend radius at all. A blend radius left on a hard node SHALL contribute nothing to that pad.

`Paint` and the extended modes SHALL keep the drag they have. Paint's colour fades over `max(support, k)` whatever the profile, and the extended modes ignore the profile by design and measure their own support.

This SHALL NOT be applied to a node's OWN bound, which keeps its `max(support, k)` dilation. In a mixed chain that dilation is margin for the drag a node's smooth neighbours apply to a running value it contributed to, and removing it measured 540 to 10,105 band-clamped disagreements against the full tape on a 12-node hard/smooth document — against 540 to 540, exactly, for narrowing the pad alone.

The narrowing SHALL NOT move any value: band-clamped results stay identical to the full tape over a document mixing hard and smooth combines in one chain, sampled widely enough that a change in what the cull drops is visible.

#### Scenario: A hard node sets no pad
- **GIVEN** a layer whose every node carries a hard profile and a non-zero blend radius
- **WHEN** the cull pad for that layer is computed
- **THEN** it is zero — and the same layer blended smoothly pads by `min(support, k * envelope(N))`, that profile's support clamped down by the chain-pad envelope at the layer's effective contributor count, so the two are distinguishable

#### Scenario: The largest drag is the smooth one
- **GIVEN** a chain alternating a hard node with the layer's LARGEST blend radius and smooth nodes with a smaller one
- **WHEN** the cull pad is computed
- **THEN** it is the smooth drag, not the hard node's radius

#### Scenario: A node's own bound is unchanged
- **GIVEN** a node with a hard profile and a non-zero blend radius
- **WHEN** its geometric bound is read
- **THEN** it is still dilated by that radius, because a mixed chain needs the margin

#### Scenario: Paint and the extended modes keep what they drag
- **WHEN** the pad is computed for a layer holding a hard-profile Paint or an extended mode with a blend radius
- **THEN** that radius still contributes, because the colour fade and the channel both read it

#### Scenario: The field is unchanged
- **GIVEN** a document mixing hard and smooth combines in one chain, the hard ones carrying the larger blend radius
- **WHEN** a per-brick culled tape is evaluated against the whole-document tape inside the brick
- **THEN** every band-clamped value is equal exactly, over a sweep wide enough that the narrowed cull is exercised

### Requirement: A cached cull index is extended in place only while unobserved
A cached cull index MAY be extended in place instead of being copied, and SHALL be copied whenever any other holder of it exists. The copy protects a reader holding the index against a plan it already made; a reader takes its handle under the same lock the extension runs under and holds it while it reads, so the absence of any other handle, observed under that lock, is what makes extending in place safe.

#### Scenario: A stroke extends the cached index without copying it
- **WHEN** consecutive appends are absorbed by a document whose cull index nothing else is holding
- **THEN** no copy of the index is made, and every read gets the index a rebuild would give

#### Scenario: A held index is not mutated under its holder
- **GIVEN** a caller holding the cull index
- **WHEN** an append is absorbed
- **THEN** the caller's index is unchanged and the cache takes a copy to extend

### Requirement: An added layer may name the content it shares
The layer-add command SHALL be able to name a CONTENT SOURCE layer instead of carrying the content itself, so that creating an instance costs a reference wherever the command travels — the undo stack, the journal and the document file alike.

Applying such a command SHALL resolve the named layer and share its content. It SHALL refuse when the id does not resolve, rather than fall back to an empty or copied edit list: a replay that silently produced an unshared layer would reintroduce, one recovery later, exactly the multiplication the format change removes.

A command that carries content SHALL be applied unchanged, so the in-memory path — where the command already holds the shared pointer — is unaffected.

#### Scenario: Replaying an instance creation shares
- **WHEN** a journal containing an instance creation is replayed onto the snapshot it was taken against
- **THEN** the replayed instance shares the source layer's content rather than holding a copy

#### Scenario: A named source that is gone refuses
- **WHEN** a layer-add command naming a content source is applied to a document without that layer
- **THEN** the command is refused and the document is unchanged

### Requirement: Reinserting an existing layer names the content it shares
A command pair that REMOVES an existing layer and adds it back — a reorder, and the sever a consolidation performs — SHALL decide, before the remove, whether the layer's edit list is shared, and the add SHALL name a surviving sharer when it is.

In memory the add already carries the layer's own shared pointer and the name changes nothing. The name is what makes the pair correct once SERIALIZED: an add that names no source writes the edit list inline, so a replay of a reorder deserializes it as private content and unlinks the layers. An add whose content is deliberately private — the consolidation's sever — SHALL name nothing and keep the inline form.

#### Scenario: A reordered instance replays as an instance
- **WHEN** a journal containing a reorder of a shared layer is replayed onto the snapshot it was taken against
- **THEN** the reordered layer still shares the other's edit list

### Requirement: Consolidation gives a shared layer its own content first
Consolidating a layer SHALL affect that layer only. When the layer's edit list is shared with other layers, consolidation SHALL first replace it with a private copy, and that replacement SHALL be part of the same undo step as the bake so that undoing the consolidation restores the shared content.

#### Scenario: A bake does not reach the other sharers
- **WHEN** one of two layers sharing an edit list is consolidated
- **THEN** the other layer's edit list is exactly what it was

#### Scenario: Undo restores the sharing
- **WHEN** the consolidation of a shared layer is undone
- **THEN** the layer's content is the shared content again, and an edit through either layer is visible in both

### Requirement: The chain pad grows with the chain it compiles, clamped at support
A smooth distance blend's contribution to the layer's cull pad SHALL be `min(support, k * envelope(N))`: a measured per-profile envelope in k-multiples that rises with `N`, clamped at the profile's support so the pad never exceeds the pre-envelope `max(support, k)` anywhere.

`N` SHALL be the layer's EFFECTIVE contributor count: the node-map size times the symmetry multiplicity `1 + popcount(mirror_axes) + max(0, radial_count - 1)` — the compiler emits a mirrored item once per mirror and radial copy, each copy a real contributor to the layer's one serial chain, and the modes compose additively. Counting the map alone under-pads an amplified chain and is a refuted proxy.

The SEAM blends the symmetry copies enter through — the layer's mirror and radial blend radii, independent of any item radius — SHALL fold into the pad as a quadratic chain term of the largest seam radius, clamped at the ceiling the item blends alone resolve to (the pre-envelope pad). Where the seam demands more than that ceiling, the pad SHALL equal the pre-envelope pad exactly.

The pad SHALL resolve at read time from raw per-profile maxima and live counts, so that an incrementally appended cull index resolves exactly what a fresh build reports, and every input only rises under an append. A symmetry edit is not an append and rebuilds the index.

The envelope is evidence-bound: it holds at least the knees' seed-draw drift above every measured knee, and it SHALL NOT be lowered without a sweep of comparable breadth. The correctness bar is relative — equal-or-fewer band-clamped in-band disagreements than the pre-envelope pad, per config — because no fixed dilation is a proof for an arbitrary chain.

#### Scenario: The pad rises with the chain
- **GIVEN** two layers blending at the same radius and profile, one map of 75 nodes and one of 1200
- **WHEN** their cull pads are computed
- **THEN** the longer chain pads wider, and neither pad exceeds the profile's support

#### Scenario: A symmetric layer pads by the chain it compiles
- **GIVEN** a 75-node layer under radial count 64, its items participating in the symmetry
- **WHEN** the cull pad is computed
- **THEN** the envelope resolves at ~4800 effective contributors, not 75 — and per-brick culled values stay band-clamp identical to the full tape where resolving at 75 measured real disagreements

#### Scenario: A seam wider than any item blend still pads
- **GIVEN** items blending at radius k under a layer seam radius of 3k
- **WHEN** the cull pad is computed
- **THEN** it equals the ceiling the item blends resolve to — the pre-envelope pad — never wider, and the culled values stay band-clamp identical to the full tape

#### Scenario: Never wider than the pre-envelope pad
- **GIVEN** any layer, symmetric or not, seams or not
- **WHEN** the cull pad is computed
- **THEN** it is at most the pre-envelope `max(support, k)` pad, and where the clamp binds the culled tape is bit-identical to that pad's compile

#### Scenario: An appended index resolves what a fresh build reports
- **GIVEN** a cull index built before a stroke and extended by its appended dabs
- **WHEN** the pad is resolved after the append
- **THEN** it equals the pad a fresh build of the grown document reports, exactly

### Requirement: The undo history reverses a topology gesture
The one undo history SHALL reverse a gesture that changed a surface's connectivity, alongside the scene commands, voxel passes, mask edits, surface-group edits and vertex deltas it already reverses.

The step SHALL carry a sparse topology delta rather than a snapshot, and a gesture SHALL be one step however many stamps and topology operations it contained.

The history SHALL reach the surface through a RESOLVER supplied by the owner above it, as the existing kinds do, because the module layering forbids `scene` from naming `mesh` at all.

A compound step SHALL be able to span a scene command and a topology gesture, so a crossing that creates a layer and sculpts into it undoes as one.

A journal SHALL encode, decode and replay the new kind, and a journal written before this change SHALL still replay.

#### Scenario: An adaptive stroke undoes as one step
- **WHEN** a stroke that split, collapsed and flipped edges is undone
- **THEN** the surface is bit-identical to before the stroke and one step was consumed

#### Scenario: An older journal still replays
- **WHEN** a journal recorded before this change is replayed
- **THEN** it applies exactly as it did before, and no step is skipped

### Requirement: A document reports what an adaptive surface costs
The memory roll-up SHALL account for an adaptive surface separately from the flat mesh layers, and SHALL separate its authoritative content from its rebuildable caches — the spatial partitions, derived normals and preview staging.

A host answering a memory warning needs to know which part it may release. Authoritative topology and attributes SHALL never be reported as rebuildable.

#### Scenario: The roll-up separates content from cache
- **WHEN** a document holding an adaptive surface with a built spatial index is measured
- **THEN** the report names the authoritative surface bytes and the rebuildable cache bytes separately

### Requirement: The undo history reverses a multires gesture
The one undo history SHALL reverse a gesture made at any level of a subdivision hierarchy, through a step carrying sparse per-level detail changes and reaching the surface by a resolver supplied from above.

Level lifecycle operations — adding a level, removing the highest level, switching the sculpt level — SHALL be reversible where they change persistent state, and SHALL be recorded as a barrier only where they genuinely destroy information, with what happened named for a host to show.

A journal SHALL encode, decode and replay the new kind, and journals written before this change SHALL still replay.

#### Scenario: A gesture at a level undoes as one step
- **WHEN** a stroke at the active level is undone
- **THEN** the reconstructed surface is bit-identical to before the stroke and one step was consumed

#### Scenario: Removing a level is not silently irreversible
- **WHEN** the highest level is removed
- **THEN** the operation is either reversible or recorded as a barrier naming what was lost

### Requirement: A hierarchy reports what it costs, separated by what may be released
The memory report for a multiresolution surface SHALL account for its authoritative content — the base, the hierarchy metadata and the per-level detail — separately from its rebuildable caches: reconstructed positions for inactive levels, per-level adjacency, per-level spatial indices and preview staging.

A host under memory pressure SHALL be able to identify what it may release. Authoritative detail SHALL never be reported as rebuildable.

The accounting is PER SURFACE rather than a row in the document roll-up, and that is a consequence of where a hierarchy lives rather than a weakening. A multiresolution surface is a standalone handle that no `scene::Layer` owns — exactly as an adaptive surface is — so `io::document_memory` cannot see one to report it, and inventing a layer home for this representation ahead of the one that shipped before it would be the wider change, made here for the narrower reason. A host holds the surface beside its document and adds the two figures.

#### Scenario: The report separates detail from cache
- **WHEN** a hierarchy with several built level caches is measured
- **THEN** the report names the authoritative detail bytes and the rebuildable cache bytes separately, and releasing the caches changes the second and not the first

### Requirement: A whole-mesh replacement is one undo step
The undo history SHALL be able to record the replacement of one mesh layer's entire geometry as a single reversible step, carrying the mesh on each side of the replacement.

A sparse vertex delta SHALL NOT be used for this. A delta records no indices — that is the fixed-topology contract it exists to serve — so it cannot express a change that replaces every vertex and every polygon, and applying one recorded against the old geometry to the new geometry is a corruption rather than an undo.

The step's cost SHALL be counted by the history's byte accounting, so the budget that evicts oldest steps can see the largest kind there is. The step SHALL survive a crash through the journal, so a recovered document does not silently lack a rebuild the artist watched happen.

A replacement that changed neither the vertex count, the triangle count, nor any position SHALL be dropped rather than recorded, as every other recorder drops a no-op.

#### Scenario: One rebuild is one step, and it reverses exactly
- **WHEN** a mesh layer is rebuilt through the document with undo enabled
- **THEN** the undo depth grows by exactly one, undoing restores the layer's previous triangles, and redoing restores the rebuilt ones
- **AND** undoing a second time after a redo works, because applying the step did not consume it

#### Scenario: A refused or cancelled rebuild adds no step
- **WHEN** a rebuild is refused for a resource budget, refused for a protected layer, or cancelled
- **THEN** the layer's triangles are unchanged and the undo depth is unchanged

### Requirement: A mesh layer carries a geometry revision
A document SHALL expose, per mesh layer, a revision that changes when the layer's geometry is REPLACED wholesale and does not change when the layer is sculpted.

The distinction is the point. A vertex-displacement brush leaves the topology alone, which is what lets an adjacency, a spatial index or a live sculpting session remain valid across it; a wholesale replacement invalidates all three. A revision that moved for both would force every consumer to rebuild after every brush stroke, and one that moved for neither would not exist.

#### Scenario: A sculpt does not move it and a rebuild does
- **WHEN** a mesh layer is stamped with a displacement brush and then rebuilt
- **THEN** the revision is unchanged after the stamp and changed after the rebuild

### Requirement: A stale result cannot overwrite newer work
Where a caller performs a long rebuild outside the document and commits the result afterwards, the commit SHALL accept the revision the caller read before starting, and SHALL be refused if the layer's geometry has been replaced since.

A refusal SHALL leave the layer byte-identical. A caller that knows nothing could have intervened MAY skip the check explicitly; skipping it SHALL be something the caller asks for rather than the default that happens when it forgets.

#### Scenario: The commit that arrived late is refused
- **WHEN** a caller reads a layer's revision, rebuilds outside the document, the layer is rebuilt by something else in the meantime, and the caller then commits
- **THEN** the commit is refused, and the layer still holds the newer geometry

#### Scenario: The same commit at the current revision is accepted
- **WHEN** the caller re-reads the revision and commits at it
- **THEN** the commit succeeds

### Requirement: A cached surface session is refused after its layer is rebuilt
A sculpting session held over a mesh layer SHALL refuse to operate once that layer's geometry has been replaced, including when the replacement happens to have the same vertex and triangle counts.

Comparing counts is not sufficient and neither is comparing the address of the layer's mesh: a replacement that lands on the same counts passes both, and the session's cached adjacency and spatial index then describe triangles that no longer exist. Every operation after that moves the wrong vertices with no error.

#### Scenario: A same-count replacement is still caught
- **WHEN** a mesh layer is replaced by a mesh with identical vertex and index counts but different connectivity, under a live sculpting session
- **THEN** the next operation on that session is refused rather than applied

### Requirement: A bake can be merged into a region of a layer
A host SHALL be able to bake a REGION of a layer into one volume and put it back where the items it absorbed were, leaving every item outside parametric. Collapsing the whole layer SHALL NOT be the only way to install a bake.

The region a merge absorbs and samples SHALL be the INFLUENCE CLOSURE of the caller's region: the region grown, to a fixed point, until every item whose influence bound meets it is wholly inside it. Absorbing merely the items that overlap the caller's region is NOT sufficient — an item straddling the edge remains, and material it had carved returns where the installed volume cannot remove it again.

At the closure, the field outside the box SHALL be unchanged, and inside the box the bake SHALL be the whole answer, because no remaining item contributes there.

An item whose influence is unbounded SHALL pull the closure out to the whole layer. Where the closure reaches every visible root the operation IS a whole-layer consolidation, and that SHALL be reported rather than hidden.

The merge SHALL be ONE undo step whose inverse restores the absorbed items with their ids, parameters, colours and deformers, SHALL leave hidden items alone, and SHALL be refused on a protected layer before anything is sampled.

A host SHALL be able to ask what a merge would absorb, and over what box, without baking anything.

#### Scenario: The closure takes what the region reaches and no more
- **WHEN** a region is planned over one of several well-separated items
- **THEN** it absorbs that item alone, and the sampled box does not reach the others

#### Scenario: The closure is a fixed point, not one pass
- **WHEN** an item earlier in the edit list is only reached after a later item widens the box
- **THEN** it is absorbed too

#### Scenario: The field outside the merged region does not move
- **WHEN** a region of a layer is merged
- **THEN** the surface outside the closure is where it was, and the merged region still has its own surface

#### Scenario: Working one patch repeatedly does not stack volumes
- **WHEN** the same patch is merged once per gesture over several gestures
- **THEN** the layer holds one baked item for that patch, not one per gesture

#### Scenario: A whole-layer closure says so
- **WHEN** the closure reaches every visible root
- **THEN** the merge reports that it consolidated the layer

#### Scenario: It undoes to the parametric form
- **WHEN** a region merge is undone
- **THEN** the absorbed items are back and the field is what it was

### Requirement: A document part may exclude one layer

The document-part compile SHALL offer a third selection beside "the visible SDF
layers before the active one" and "only the active one": **every visible SDF
layer except the named one**, joined by the same hard union a whole-document
compile emits between layers.

The excluded layer SHALL be excluded wherever it sits in the stack, not only
when it is last. A selection that stopped at the named layer would be the
existing "before" case under another name, and would silently drop every layer
above it.

The excluded part SHALL cull under the WHOLE DOCUMENT's pad, on the same terms
and for the same reason the other two parts do: a part compiled under its own
smaller pad drops items the whole-document compile keeps, and the parts then no
longer sum to the whole.

Excluding a layer that is hidden, that is not an SDF layer, or that the document
does not hold SHALL be an ordinary compile of every visible SDF layer — the
compile itself has nothing to refuse, because such a layer contributes nothing
to the union in the first place. Refusing a caller who names one is the C ABI's
to do, where the caller's intent is known.

#### Scenario: The excluded layer is in the middle of the stack
- **WHEN** a document with three visible SDF layers is compiled excluding the middle one
- **THEN** the tape evaluates to the hard union of the first and third, and the middle layer's items appear in it nowhere

#### Scenario: The parts sum to the whole
- **WHEN** a document is compiled excluding one layer, and that layer is compiled on its own
- **THEN** the hard union of the two values equals what a whole-document compile evaluates to at the same points

#### Scenario: An excluded part culls under the document's pad
- **WHEN** a part excluding one layer is compiled under a cull region
- **THEN** it keeps every item a whole-document compile under that region keeps, so the two parts still sum to the whole

#### Scenario: Excluding the only visible layer leaves empty space
- **WHEN** a document whose one visible SDF layer is the excluded one is compiled
- **THEN** the compile produces a tape that evaluates as empty space rather than failing

### Requirement: A candidate influence bound is ranked on measurement

A change that TIGHTENS an influence bound SHALL be justified by a measurement that distinguishes the bound it ships from the tighter one it rejects, not by reasoning alone. The property test SHALL sample densely enough that a bound which is too small produces a non-zero drift.

The sample count is a property of how RARE a violation is rather than of how hard the document is. A local item's bound is its own geometry and a violation there is dense; a non-local item's violation can be one sample in ten thousand, so the non-local cases SHALL sample at a rate that finds it every run.

#### Scenario: A too-small bound is caught
- **WHEN** the property test runs against an intersect's own geometry bound instead of the bound the engine reports
- **THEN** it reports a non-zero band-clamped drift outside that box

#### Scenario: The shipped bound holds
- **WHEN** the same test runs against the bound the engine reports
- **THEN** the drift is exactly zero over the same samples

### Requirement: A coarse cull plan may precompute its test but not change its answer
The coarse cull MAY fold per-entry constants — whether an item's influence is local, and whether its bound is infinite — into a form decided when the entry is cached rather than re-derived per plan, and MAY hold that form apart from the survivor records so the scan reads only what it tests. Both clauses are settled once the entry exists, and re-deriving them dominated the scan: the scan is the plan, at 0.1373 ms against 0.1379 ms for the bare predicate loop over 50,000 entries.

The survivors a plan reports SHALL be exactly those the survive test reports, in chain order, for EVERY region — including the degenerate ones. A folded test that is exact only over ordinary regions is not exact, because the region a plan is asked about comes from the caller.

An entry whose own geometry bound is EMPTY SHALL be culled by every region, as the survive test culls it. This is the case a fold gets wrong: such an entry is local and its bound is not infinite, so a fold that stores bounds and tests them with a bare intersection stores an empty box, and an infinite region passes a bare intersection against ANY box. Empty geometry bounds are reachable — a stroke or armature with no points, a volume with no payload — so this SHALL be tested rather than argued.

An EMPTY region needs no such exception and SHALL NOT be given one, since a bare intersection against it passes only a box that is infinite on every axis, which is what the fold gives the entries that can never be culled and is what the survive test returns there.

#### Scenario: A folded scan answers what the predicate answers
- **GIVEN** a chain holding non-local items, items with infinite bounds, items with empty bounds and ordinary ones
- **WHEN** it is planned against an empty region, an infinite region, a region over part of it and a region over none of it
- **THEN** each plan's survivors are the survive test's survivors, in chain order

#### Scenario: An item with no geometry is culled by a region containing everything
- **WHEN** a document holding a strokeless stroke, a boneless armature and a payload-less volume is planned against an infinite region
- **THEN** none of the three survives, and every item with a real bound does

#### Scenario: An extended index scans what a rebuilt one scans
- **WHEN** an index is extended by an append and another is built fresh over the same document
- **THEN** the two plans report the same survivors for the same region

### Requirement: A per-brick cull decides from the cached entry, not from the node
Where a coarse plan supplies a chain's survivors, the per-brick cull SHALL decide whether a survivor reaches this brick from the cached entry alone — its locality flag and its bound — and SHALL NOT reach the node behind it to do so. Both values were computed when the chain was cached and cannot have changed while the index is valid, and the node is reached through a pointer into the layer's node map, so re-deriving them costs a cache miss per rejected item: about 12,000 of them per brick, over 24 bricks, on a 50,000-item document.

The decision SHALL remain the compiler's own — a survivor reaches the brick unless it is local, finite and misses the region — and SHALL cover groups and items alike, a group's cached entry being local with an infinite bound exactly where its subtree is non-local.

An entry a plan supplies SHALL be visible, so the per-brick cull need not check: a chain caches only the children it would compile.

#### Scenario: A planned per-brick tape is the tape the plain compile gives
- **WHEN** each brick of a batch is compiled through an index and a plan, and again with a plain cull region and no index
- **THEN** the tapes are byte-identical, including for chains holding groups, non-local items and infinite bounds

#### Scenario: A plan handed in without a cull region is dropped
- **WHEN** a document is compiled with an index and a plan but no cull region
- **THEN** the plan is ignored and the tape is the whole document's, since a plan without a region could only mean a pruned whole-document tape

#### Scenario: The saving is in the rejects
- **WHEN** a dab's bricks are compiled over a batch survivor list far larger than any one brick keeps
- **THEN** what a rejected survivor costs is the cached test alone

### Requirement: A captured region of the field is a reusable asset

A finite region of a document's field SHALL be capturable as a self-contained
signed-field asset that can be placed into an SDF layer many times. A capture
SHALL be a sampled field rather than a copy of the edit items that produced it:
a captured subtree carries identity dependencies on nodes that may be edited or
deleted, an evaluation cost that grows with what it captured, and no bounded
serialized form, and none of those is true of samples.

A placement SHALL be an ORDINARY EDIT ITEM — editable, transformable, undoable,
and combinable with the same ops and blends every other item has. The feature is
non-destructive because of that and not in addition to it.

**Placing an asset SHALL NOT copy its samples.** A document with a thousand
placements of one asset SHALL hold one copy of that asset's payload, and the
per-placement cost SHALL be a transform and a reference. Anything else makes a
detail brush unusable at the scale a detail brush is for.

A capture SHALL be taken in a frame the caller supplies, and the asset SHALL
record it, so the same asset placed at a new orientation is the shape that was
captured. The frame SHALL NOT be inferred from the captured content: an
orientation derived from the samples changes when the region moves, so
re-capturing the same detail would produce an asset that no longer agrees with
the placements already made from it.

Scale SHALL be uniform where the field's exactness contract requires it, and a
scale the contract cannot honour SHALL be refused rather than accepted with a
field that quietly reports the wrong distance — a marcher stepping on a wrong
bound misses surface, which is visible as holes rather than as an error.

Intensity SHALL NOT be expressed by multiplying the captured distance. That
scales the metric rather than the sculpt, so the result is a field whose zero set
has moved and whose gradient no longer has unit length; scale, the combine op and
the blend are the controls that mean what an artist expects.

#### Scenario: An asset reloaded on its own is the asset that was saved
- **WHEN** a captured asset is written to its standalone form and read back
- **THEN** the two place identically, and the field they contribute agrees at every point

#### Scenario: A placed asset reproduces what was captured
- **WHEN** a region is captured and the asset is placed back at the transform it was captured from
- **THEN** the field it contributes agrees with the source over that region within the sampling tolerance the capture declares

#### Scenario: A thousand placements hold one payload
- **WHEN** one asset is placed many times in a document
- **THEN** the document's authoritative bytes grow by a per-placement reference and not by the asset's samples, and saving and reloading preserves that

#### Scenario: A placement is an ordinary item
- **WHEN** an asset has been placed
- **THEN** it can be moved, re-combined, hidden and undone exactly as any other item, and one gesture that places several is one undo step

#### Scenario: A scaled placement is still safe to march
- **WHEN** a placed asset carries a non-uniform scale and the field is sampled outside its surface
- **THEN** stepping from any such point by the distance the field reports does not cross the surface

#### Scenario: An asset outlives what produced it
- **WHEN** the items a region was captured from are edited or deleted
- **THEN** every placement of the asset is unaffected, because the asset holds samples rather than a reference to those items

### Requirement: A layer may be an imported mesh
A layer SHALL optionally be of `mesh` kind, carrying one imported mesh stored beside the document and keyed by layer id rather than inside the evaluated document. Its geometry SHALL be exactly what the importer returned — no welding, reordering, renormalizing or reindexing on the way in — so that what a document carries is what was imported.

Its presence SHALL NOT change what the document evaluates to. A mesh layer SHALL NOT be compiled into a tape, SHALL NOT participate in any blend, and SHALL NOT contribute to influence bounds or per-brick culling. Keeping the geometry out of the evaluated document makes that structural rather than a property to be maintained, as it already is for masks and voxel grids.

A mesh layer SHALL carry the same layer properties as any other: name, order, transform, visibility, ghost and lock. Creating and removing one SHALL go through the command vocabulary, so both are undoable and both serialize with the document.

#### Scenario: Evaluation is unchanged by a mesh layer
- **WHEN** a mesh layer is added to a document and the document is evaluated
- **THEN** the field is bit-identical to the same document without the mesh layer, and every compiled tape is unchanged

#### Scenario: The mesh is carried, not resampled
- **WHEN** a mesh is attached to a document and read back
- **THEN** its positions, normals, colors, uvs and indices are the arrays the importer produced, element for element

#### Scenario: Adding a mesh layer is undoable
- **WHEN** a mesh layer is added and the edit is undone
- **THEN** the layer is gone and the document matches what it was

#### Scenario: Removing a mesh layer does not discard its geometry
- **WHEN** a mesh layer is removed and the removal is undone
- **THEN** the layer returns carrying the same mesh, because the payload is keyed by layer id and is never erased on removal

### Requirement: A mesh layer's transform is applied by whoever consumes it
A mesh layer's vertices SHALL be stored in the space the importer produced, and its `Layer` transform SHALL be applied by whatever reads or exports the mesh rather than baked into the stored geometry. This is the rule voxel content already lives under; a mesh layer states it rather than inheriting it silently.

The transform SHALL be the existing layer transform, edited through the existing command, so that moving a mesh layer is undoable, serializes with the document, and is refused on a locked layer exactly as any other layer edit is.

Unit and axis conversion SHALL be resolved at import by baking a uniform scale into the vertices, as the FBX importer already does when it normalizes to metres. Non-uniform scale is not expressible in a layer transform and SHALL NOT be approximated.

#### Scenario: The stored mesh does not move
- **WHEN** a mesh layer's transform is changed
- **THEN** the stored vertices are unchanged, and an export of that layer places them under the new transform

#### Scenario: A locked mesh layer refuses the edit
- **WHEN** a mesh layer is locked and its transform is set
- **THEN** the edit is refused, as it is for any other locked layer

### Requirement: A group is a sub-expression
A group's children SHALL compile against a fresh accumulator and combine with the chain outside the group as one value, through the group's own op, blend and rounding. An op inside a group therefore SHALL NOT reach anything outside it.

A group SHALL have no transform of its own: the compiler composes `layer.xform * item.xform` and nothing else, so a transform on a group would change nothing. The bindings SHALL refuse to record one rather than accept an edit that is undoable and saved but inert. For the same reason a group's rounding SHALL scale by the layer's scale alone, where an item's scales by the layer's and its own.

A group SHALL NOT carry a transition op: the compiler emits no transition parameters for a group, so one would morph on defaults the node never stated.

#### Scenario: An intersect stays inside its group
- **WHEN** a group holds a shell and a cutter combined with intersect, and the layer already holds other geometry
- **THEN** the intersect trims the shell only, and the group's result combines with the rest through the group's own op

#### Scenario: A group's transform is refused rather than ignored
- **WHEN** a host sets a transform on a group
- **THEN** the edit is refused and the document is unchanged

### Requirement: An inline group is its children, exactly
A group carrying the inline op SHALL compile its children against the OUTER accumulator, so the field is bit-identical to the same children added directly in the same order. Its own blend, rounding and colour are never read, and the bindings SHALL refuse them rather than accept values that cannot take effect.

#### Scenario: Inline and flat agree bit for bit
- **WHEN** the same ordered edits are compiled once under an inline group and once at the layer root
- **THEN** the two fields are identical at every sampled point, not merely close

### Requirement: A group with nothing to combine emits nothing
A group whose op carves (subtract, intersect, the extended modes other than the material-creating ones) and that has no accumulated value beneath it SHALL emit no instructions, as a carving item in the same position does. A group whose children all turn out to be invisible or culled SHALL likewise emit nothing: any partial emission SHALL be rolled back, so the tape is identical to one compiled without the group at all.

#### Scenario: A carving group first in a chain
- **WHEN** a subtract group is the first node in a layer
- **THEN** the field is the empty field, not a hole in it

#### Scenario: A group whose subtree compiled to nothing
- **WHEN** a group's children are all hidden or culled
- **THEN** the compiled tape and the evaluated field are identical to the document without that group

### Requirement: A node cannot become its own descendant
Reparenting SHALL refuse to move a node into its own subtree, and SHALL leave the tree untouched when it refuses. A move that closed such a cycle would detach the subtree from the root list, so it would stop evaluating, be dropped by the next save (serialization walks from the roots) and be unreachable by removal.

#### Scenario: A group moved under its own descendant
- **WHEN** a group is moved into one of its own children or grandchildren
- **THEN** the move fails, the node keeps its parent and index, and the subtree still evaluates

### Requirement: A group's influence bound covers its own combine
A group's influence bound SHALL be the union of its children's bounds dilated by what the group's own combine reaches — for an extended op its documented support, and otherwise the greater of the blend profile's support and the blend radius, which is what the item path already uses. A hard profile supports zero distance while a paint combine still fades over the radius, so the profile's support alone is not conservative.

#### Scenario: A paint group with a hard profile
- **WHEN** a group combines with paint, a hard profile and a non-zero radius
- **THEN** its influence bound is dilated by at least that radius

### Requirement: A warp is authored in the space the deformer chain runs in
An item's deformer chain runs on the point AFTER the whole inverse has been applied — the per-axis scale included, because it is innermost. Any tool that authors a deformer from world-space input SHALL therefore map its input through that same whole inverse, and not through the placed frame alone.

Composing only the layer and node transforms SHALL NOT be treated as the item's frame. It was the whole story before an item could carry a per-axis scale and is not one now: a world point on the surface of an item stretched by a factor maps, under the placed frame alone, to a local point that far outside the primitive, and a falloff authored there reaches nothing.

A drag on the surface of a stretched item SHALL produce the same local warp as the same drag on the corresponding point of the unstretched one, since the local geometry is identical and only the frame differs.

#### Scenario: A drag reaches a stretched item's surface
- **WHEN** an item scaled by a factor on one axis is dragged at the world point where its surface now sits
- **THEN** the surface moves, where before the tool produced a warp that reached no part of the item

#### Scenario: Stretched and unstretched drag identically
- **WHEN** the same drag is applied to a stretched item and to the unstretched one, each at its own corresponding surface point
- **THEN** both produce the same local grab centre and the same resulting field value at the dragged point

### Requirement: A scalar falloff under a non-uniform frame never over-reaches
A grab carries ONE radius, and a non-uniform frame maps the caller's world-space sphere to a local ellipsoid, so no scalar radius is exact. The radius SHALL be divided by the LARGEST scale factor, so that every world-space reach is at most the radius the caller named.

The direction SHALL be documented at the call as a choice, not left as arithmetic: the opposite division is equally arithmetic and takes geometry the caller did not enclose. Under-reach is recoverable by dragging again; over-reach is not, and it is the same conservatism the non-uniform distance operator applies when it multiplies by the smallest factor.

#### Scenario: A drag stays inside what was circled
- **WHEN** an item stretched by a factor of four is dragged with a given radius
- **THEN** the widest world-space reach of the resulting falloff does not exceed that radius, and geometry outside it is unchanged

### Requirement: An item carries a per-axis scale
A placed item SHALL carry a per-axis scale in addition to the uniform one its transform already holds, so that a primitive whose extents differ per axis — a squashed capsule, a squashed cylinder, a box stretched on one axis — is expressible after placement and not only at creation.

The two scales SHALL MULTIPLY rather than replace one another: the transform's factor stays the uniform similarity factor and the per-axis scale modulates it, so setting either leaves the other where it was and the order they are set in does not matter.

The per-axis scale SHALL be applied INNERMOST — in the item's own local frame, inside its rotation and position — so that the composed map is `layer transform * item transform * per-axis scale`.

It SHALL NOT be a field of the transform type. That type is a SIMILARITY, and its algebra is closed because of it: the product of two is another and the inverse exists in closed form. A non-uniform scale does not commute with rotation, so widening the transform would make every composition in the engine a general matrix and take the exactness bookkeeping with it. Innermost and node-local, it composes as one matrix multiply at the places that build a matrix from an item.

Every component SHALL be greater than zero. A zero collapses the item onto a plane and has no inverse; a negative component mirrors it, which the layer mirror already expresses and which would flip the winding of a boolean without saying so.

#### Scenario: A primitive is stretched after it is placed
- **WHEN** a unit sphere is placed and given a per-axis scale of (2, 1, 1)
- **THEN** its surface crosses x at 2 and y at 1, and its geometry bound reports the same extents

#### Scenario: The two scales multiply
- **WHEN** an item is given a uniform scale of 2 and a per-axis scale of (1.5, 1, 1)
- **THEN** its effective scale is (3, 2, 2), whichever order the two were set in

#### Scenario: A degenerate scale is refused
- **WHEN** a per-axis scale with a zero or negative component is set
- **THEN** the edit is refused and the item is unchanged

### Requirement: A non-uniform scale costs exactness and not step size
Evaluating a per-axis scale SHALL divide the sample point by the three factors and multiply the resulting distance by the SMALLEST of them, which never overestimates the true distance.

The field SHALL therefore remain 1-Lipschitz, and the reported Lipschitz bound and safe step scale SHALL NOT move: a marcher takes the steps it always did and nothing gets slower. What the field SHALL lose is exactness — the value becomes a BOUND on the distance rather than the distance, short by at most the ratio of the largest factor to the smallest — and that loss SHALL be recorded in the compiled field's classification so a consumer that reads the value AS a distance can tell.

A per-axis scale whose components are all equal SHALL be treated as the similarity it is: the field stays exact, and it SHALL compile to tape identical to the uniform scale of the same factor. The default SHALL be `(1, 1, 1)`, so a document that never sets one SHALL compile to exactly the tape it compiled to before this existed.

#### Scenario: The reported value never overestimates
- **WHEN** the field of a squashed primitive is sampled from outside and a full step is taken along the inward ray by the reported value
- **THEN** the step never lands inside the surface

#### Scenario: Exactness goes and the step scale stays
- **WHEN** an item is given a non-uniform scale
- **THEN** the compiled field reports that it is no longer exact, while its Lipschitz bound and safe step scale are unchanged from the uniform case

#### Scenario: A uniform per-axis scale changes nothing
- **WHEN** an item is given a per-axis scale of (s, s, s)
- **THEN** the field stays exact and evaluates identically to the same item under a uniform scale of s

### Requirement: Every copy of an item is scaled with it
A layer's mirror and radial copies of an item, the sampled box of a feathered replace, and the gate that protects an item SHALL all compose the item's per-axis scale exactly as the item's own record does.

A copy that missed it would be a differently-shaped reflection of the same item; a gate that missed it would protect a region the surface no longer occupies. The bounds used for culling and for selection SHALL compose it for the same reason — a bound tight around the shape the item no longer is would let the cull drop something on screen.

#### Scenario: A mirrored squash is squashed on both sides
- **WHEN** a squashed item is placed in a mirrored layer
- **THEN** both copies have the same shape

### Requirement: Rounding follows the factor the distance follows
An item's rounding is authored in its own local units and converted to world units by the compiler. Under a per-axis scale that conversion factor SHALL be the uniform scale times the SMALLEST per-axis component — the same factor the compiled field multiplies the item's local distance by — and NOT the uniform scale alone.

The bound computed for culling and for selection SHALL use that same factor, so the dilation the bound applies and the dilation the field applies agree. They are separate code paths and agreeing is not automatic; an item whose bound dilated by more than its field does costs cull precision, and one that dilated by less drops geometry that is on screen.

#### Scenario: Rounding and its bound use one factor
- **WHEN** an item carrying a rounding is given a per-axis scale
- **THEN** the world-space rounding and the bound's dilation are both computed from the uniform scale times the smallest per-axis component

### Requirement: Undo brackets nest
Opening an undo group inside an already-open one SHALL NOT start a second step. Nested brackets SHALL collapse into the outermost one, and the step SHALL close when the outermost bracket closes.

Without this an entry point that groups its own work cannot be called from inside a caller's group without splitting one gesture into two undos — and the case is ordinary rather than exotic: consolidation brackets its own sever-and-install, and a sculpt gesture that commits a stroke and then consolidates it must be one thing for the artist to undo, in one step they can see the far side of.

An unbalanced close SHALL be ignored rather than corrupt the stack: the next command SHALL open its own step as it would have. An outermost bracket that recorded nothing SHALL still record nothing, including when it contained only empty inner brackets.

#### Scenario: An inner bracket does not open a step
- **WHEN** commands are recorded inside a bracket that itself contains a bracket
- **THEN** the stack has gained exactly one step, one undo restores the document to before the outermost bracket, and one redo restores it to after

#### Scenario: An unbalanced close is ignored
- **WHEN** a group is closed that was never opened, or closed once more than it was opened
- **THEN** the stack is intact and the next command opens its own step

#### Scenario: Empty brackets record nothing
- **WHEN** a bracket containing only another empty bracket is opened and closed
- **THEN** the stack has gained nothing

### Requirement: An already-computed volume can be installed as a layer's content
Consolidation is two things sold together — sample the layer's field into a volume, then replace the layer's edit list with that volume — and the second SHALL be reachable on its own.

A caller that already holds a volume for a layer SHALL be able to install it as that layer's single item without the layer being sampled again. The installer SHALL be the same code the collapsed form runs, so everything the collapsed form guarantees holds here: shared instance content is severed first so that baking one subtool does not collapse its duplicates; the removals and the add are ONE undo step whose inverse restores the absorbed items with their ids, parameters, colours and deformers; a protected layer is refused; the layer's own transform is left where it was authored; and the first absorbed item's colour survives onto the volume.

The installer SHALL NOT check that the volume is a plausible bake of that layer. It cannot — a volume is a volume — and the caller owns that claim.

#### Scenario: Installing a volume is what consolidating installs
- **GIVEN** two identical documents
- **WHEN** one is consolidated and the other has the volume of its own bake installed with the same parameters
- **THEN** the two documents serialize to the same bytes, and both report the layer as consolidated

#### Scenario: Installing a volume is one undoable step
- **WHEN** a volume is installed on a layer with an undo stack
- **THEN** the stack has gained one entry, undoing restores the layer's items exactly, and redoing restores the volume

#### Scenario: Installing a volume severs shared content and refuses a protected layer
- **WHEN** a volume is installed on a layer whose edit list is shared with another
- **THEN** the other layer's items are untouched, and one undo restores both the absorbed items and the sharing
- **AND WHEN** installation is attempted on a locked layer, a ghosted layer or an unknown layer
- **THEN** it is refused and the document is unchanged

### Requirement: The roll-up covers the surface representations
The document memory roll-up SHALL account for adaptive surfaces, multiresolution hierarchies and sculpt layers, each split into authoritative content and rebuildable cache, alongside the categories it already reports.

A category that is missing from the roll-up is invisible to a host answering a memory warning, and the roll-up already found one such omission — a node accounting six members behind the type it walked, missing exactly the largest things a node owns.

The roll-up SHALL be checked against the types it walks by a test that fails when a new member is added without being accounted, rather than by review.

#### Scenario: A new surface category is accounted
- **WHEN** a document holding every surface representation is measured
- **THEN** each representation contributes to the report, and the sum of the categories equals the reported total

#### Scenario: An unaccounted member fails the build
- **WHEN** a member is added to an accounted type without being included in its byte count
- **THEN** the accounting test fails

### Requirement: Layer content and layer properties are both reversible
The one undo history SHALL reverse both a stroke written into a sculpt layer and a change to a layer's properties — rename, strength, visibility, order, lock, add, remove, merge and bake.

Property changes are small and fully describable, so they SHALL be recorded as reversible steps rather than as barriers. An operation that genuinely destroys information SHALL still be a barrier, and SHALL name what it destroyed for a host to show.

Content and property payloads SHALL be separate step kinds or separately tagged, so that undo memory is measurable and journal replay can validate each.

The history SHALL reach the layer stack through a resolver supplied by the owner above it, as the existing kinds do.

#### Scenario: A slider undoes
- **WHEN** a layer's strength is changed and undone
- **THEN** the previous strength is restored and the evaluated surface matches what it showed before

#### Scenario: A stroke into a layer undoes without touching the stack
- **WHEN** a stroke written into a layer is undone
- **THEN** the layer's detail is restored and its name, order, strength and visibility are unchanged

### Requirement: A document reports what layers cost, separately from caches
The memory roll-up SHALL report sculpt-layer content separately from the evaluated caches derived from it, and SHALL report it as authoritative.

An evaluated stack cache is rebuildable and may be released under pressure. Layer content is the artist's work and SHALL NEVER be reported as rebuildable, nor released to satisfy a budget.

#### Scenario: A trim never costs a layer
- **WHEN** every rebuildable cache is released under pressure
- **THEN** the sculpt-layer content bytes are unchanged and the evaluated surface reconstructs identically

### Requirement: A layer placement carries a classification
A layer's transform SHALL be classified by what it does to the layer's field, and the classification SHALL be part of what the scene model reports about a placement rather than something a caller re-derives:

- **RIGID** — a rotation and a translation, unit scale.
- **SIMILARITY** — a rotation, a translation and a uniform positive scale.
- **GENERAL** — anything else, which in this scene model means a per-axis layer scale.

RIGID SHALL be the subset of SIMILARITY whose scale factor is exactly 1, so a caller that handles similarity handles both.

The classification SHALL be made on the CHANGE from one placement to another rather than on either placement alone: a layer already carrying a uniform scale of 2 that goes to 3 has moved by a similarity of 1.5, and asking whether a placement "is" a similarity answers about the wrong thing.

A SIMILARITY SHALL additionally require that every distance term in the layer scales with the layer. It does not in general: a layer's uniform scale multiplies an item's ROUNDING and does not multiply its BLEND RADIUS. A layer holding a smooth combine with a non-zero radius therefore SHALL classify a scale change as GENERAL, because its field after the scale is not the field before it multiplied by anything.

#### Scenario: A translate and a rotate are rigid
- **WHEN** a layer is placed with a translation, a rotation, or both, at unit scale
- **THEN** the placement classifies RIGID

#### Scenario: A uniform scale is a similarity
- **WHEN** a layer is placed with a uniform positive scale beside any rotation and translation
- **THEN** the placement classifies SIMILARITY and reports the scale factor

#### Scenario: A per-axis scale is general
- **WHEN** a layer carries a scale whose components differ
- **THEN** the placement classifies GENERAL

#### Scenario: A scale on a blending layer is general
- **WHEN** a layer whose items carry a smooth combine with a non-zero radius is placed with a uniform scale
- **THEN** the placement classifies GENERAL, and the same layer moved rigidly still classifies RIGID

### Requirement: A similarity placement moves a layer's field and nothing else
For a layer whose placement classifies RIGID or SIMILARITY, re-placing that layer SHALL change its contribution to the document only by that placement. Writing `M` for the matrix taking the old placement to the new one and `s` for its uniform scale factor:

- the layer's field afterwards SHALL equal its field beforehand composed with `M⁻¹` and multiplied by `s`, in exact arithmetic. Implementations compose the placement into each item's transform, which ROUNDS, so a consumer comparing two evaluations SHALL expect agreement to within that rounding rather than bit equality — except where the composition happens to be exact, where the agreement SHALL be bitwise;
- the layer's surface afterwards SHALL be its surface beforehand mapped through `M`;
- no other layer's contribution SHALL change, because layers combine by hard union at the document level.

This SHALL hold for the SDF representation and is what makes a preview drawn under `M` exact for that layer's own surface rather than an approximation of it. It SHALL NOT be claimed for a GENERAL placement: a per-axis scale changes the field's Lipschitz behaviour, exactly as a per-axis item scale already does.

What the guarantee does NOT cover is the mutual occlusion of the hard union while the moved layer overlaps another: two surfaces each exactly placed still interpenetrate where the union would have resolved them. A consumer relying on this SHALL be told that limit.

#### Scenario: A rigid placement moves the surface exactly
- **WHEN** an SDF layer is meshed, then placed with a rotation and a translation, then meshed again
- **THEN** the second mesh equals the first mapped through the placement matrix, to the tolerance the mesher's own lattice alignment allows, and the field values agree at points mapped through the matrix — bitwise where composing the placement into the items' transforms is exact, and otherwise to within that composition's rounding

#### Scenario: A uniform scale scales distances
- **WHEN** an SDF layer is placed with a uniform scale factor `s`
- **THEN** the field at a point equals `s` times the previous field at that point mapped back through the placement

#### Scenario: Another layer is untouched
- **WHEN** one SDF layer of a multi-layer document is re-placed rigidly
- **THEN** the other layers' fields are bit-identical at every sampled point, and the document's field differs only where the moved layer's contribution wins the hard union

#### Scenario: A per-axis scale claims nothing
- **WHEN** a layer is placed with a per-axis scale
- **THEN** the placement classifies GENERAL and the field guarantee above is not asserted for it

### Requirement: A layer carries a per-axis scale

A layer SHALL carry three scale factors beside its transform, composed
innermost in the layer's own frame — before its rotation and translation — so
that an item's world map is
`layer.xform · diag(layer.scale_axes) · node.xform · diag(node.scale_axes)`.
The layer transform's uniform factor SHALL remain the similarity scale and the
three axes SHALL modulate it, so a triple of ones is the identity.

A layer whose three factors are equal SHALL behave exactly as one carrying the
uniform factor alone, and SHALL compile to bit-identical tape.

A non-uniform layer scale SHALL be reported through the field's exactness, not
through its Lipschitz bound: the evaluated distance SHALL be multiplied by the
product of the smallest component of each per-axis scale in the composition,
which never overestimates the true distance, and the safe step scale SHALL NOT
move.

Bounds, influence bounds and picking SHALL honour the three factors. A world
radius mapped into a squashed frame SHALL be divided by the LARGEST component,
so a gesture never reaches outside the region it named.

#### Scenario: Three equal factors are the uniform layer
- **WHEN** a layer's per-axis scale is set to three equal factors
- **THEN** the compiled tape is byte-identical to the same layer carrying that factor as its uniform scale

#### Scenario: A squashed layer squashes its items
- **WHEN** a layer holding a unit sphere is scaled 3x on one axis
- **THEN** the field's zero set is an ellipsoid three units along that axis and one along the others, and the layer's reported bounds contain it

#### Scenario: The field stays marchable
- **WHEN** a layer carries a non-uniform scale
- **THEN** the tape reports itself inexact and its safe step scale is unchanged from the uniform case

#### Scenario: A drag on a squashed layer reaches its surface
- **WHEN** a surface drag is applied at a point on a layer-squashed item's surface
- **THEN** the surface moves there, and the falloff never reaches outside the radius the gesture named

### Requirement: A lattice cage refuses a frame it cannot describe

The transformed-lattice deformer carries its item-to-cage placement as a rigid
transform with a uniform scale. The map a cage actually needs is
`cage.placement⁻¹ · layer.xform · diag(layer.scale_axes) · node.xform ·
diag(node.scale_axes)`, which is a general affine map whenever either per-axis
scale is non-uniform — not a similarity, and not a similarity composed with one
diagonal either.

Until that record is widened, a lattice gizmo over a per-axis-scaled layer SHALL
be REFUSED rather than placed through a record that cannot hold its map. A cage
placed through one would warp the item in a space it does not occupy, silently
and with no error, which is worse than either the refusal or the absent feature.

The refusal SHALL name the layer's scale as the reason, so a host can offer the
uniform gizmo instead of reporting a failure it cannot explain.

A cage over a layer carrying no per-axis scale SHALL behave exactly as before.

#### Scenario: A gizmo over a squashed layer is refused
- **WHEN** a lattice gizmo is applied to a layer carrying a non-uniform scale
- **THEN** it produces no warps and reports that the layer's per-axis scale is why

#### Scenario: An unsquashed cage is unchanged
- **WHEN** a lattice gizmo is applied to a layer carrying no per-axis scale
- **THEN** the warps are the ones it produced before

### Requirement: A culled compile narrows a sampled volume

A tape compiled against a cull region SHALL carry only the part of a sampled
volume that region can read. A volume's influence bound is its whole box, so the
item cull cannot drop one and nothing else narrowed it: a tape for a single
brick carried the entire payload, which made every operation that compiles per
brick — meshing with gradient normals above all — cost the size of the volume
once per brick rather than the size of the brick.

The narrowing SHALL be exact inside the region, on the same terms the cull
already promises. A cropped volume necessarily answers differently OUTSIDE its
crop, because the evaluator clamps a query onto the sampled box; that is what a
culled tape already does with the items it drops, and it is why a caller may
only evaluate a culled tape inside the region it was culled to.

**The volume's sample lattice SHALL NOT move.** A narrowed volume keeps the
origin and the brick grid of the volume it came from, because the evaluator
locates a sample by arithmetic on that origin and moving it changes the
interpolation weights — which makes the narrowed field agree with the whole one
to within rounding rather than exactly, and the difference is invisible until
something compares bytes.

Where a region reaches every stored brick, or none, the whole volume SHALL be
emitted rather than a narrowed copy: narrowing to nothing is a different field,
and narrowing to everything is a copy for no gain.

#### Scenario: A brick's tape carries a brick's worth of samples
- **WHEN** a tape is compiled against a region the size of one brick, over a document holding a large sampled volume
- **THEN** the payload it carries is a small multiple of one brick's samples rather than the whole volume's

#### Scenario: The narrowed tape answers exactly
- **WHEN** a culled tape over a sampled volume is evaluated inside its region, within the band
- **THEN** every value equals what the whole document's tape returns, exactly rather than approximately, including under a placement that rotates, moves and scales the item

#### Scenario: Colour survives the narrowing
- **WHEN** the volume carries a colour lattice
- **THEN** the narrowed tape returns the same colours as the whole one inside the region, because a narrowing that compacted samples without compacting colours would shade correct geometry from the wrong brick

#### Scenario: A whole-document compile is untouched
- **WHEN** a document holding a sampled volume is compiled with no cull region
- **THEN** the whole volume is emitted, since there is no region to narrow to

### Requirement: A culled compile drops a warp its region cannot reach

A tape compiled against a cull region SHALL NOT carry a domain warp whose
support that region cannot reach. Such a warp is the IDENTITY over the region,
so carrying it is work done for samples it cannot affect — measured at 3.20x the
cost of the same samples with no warps at all, for twelve grabs none of which
reached them.

The dropping SHALL be sound along the CHAIN and not merely per warp. Warps apply
in authoring order, so a warp is the identity over the region only if every warp
before it was: a warp that is kept may move a point, and the region each
subsequent warp is tested against SHALL therefore be widened by the most that
warp can move one.

Only a warp with FINITE SUPPORT may be dropped. A warp whose weight merely
decays, or clamps, reaches everywhere and SHALL always be carried.

A compile with no cull region SHALL carry every warp: there is no region to test
against, and that compile is the one a whole-document evaluation and a host
upload use.

This SHALL NOT change the field. Inside the region the culled tape SHALL return
exactly what the whole document's tape returns, because a dropped warp was the
identity there.

#### Scenario: A region no warp reaches carries none
- **WHEN** a tape is compiled against a region outside every warp's support
- **THEN** it carries no warps, and evaluates to exactly what the whole document's tape does over that region

#### Scenario: A region a warp reaches keeps it
- **WHEN** the region is within a warp's support
- **THEN** that warp is carried, and the field is unchanged

#### Scenario: A warp is judged after the warps before it have moved the point
- **WHEN** a warp that the region reaches displaces points toward a second warp the region alone does not reach
- **THEN** the second warp is carried too, and the field is unchanged

### Requirement: A document says when a command has changed it

A document SHALL carry a serial that advances whenever a command changes it, so
that a consumer can tell the document before an edit from the document after
one.

It SHALL advance where commands are APPLIED rather than where a binding
invalidates its caches. Those are different moments: a binding invalidates once
an edit is finished, which is after both sides of that edit have been examined,
so a consumer keyed on the later moment cannot distinguish them and will answer
a question about the new document with the old one's geometry. A bound derived
that way is too small, and a bound that is too small is under-invalidation —
which renders as stale geometry rather than as an error.

It SHALL advance for every command-based mutation without those mutations being
enumerated, so that undo, redo and a replayed journal are covered by
construction and a command added later cannot be forgotten.

It SHALL NOT advance for a command that was refused, since such a command leaves
the document unchanged.

A mutation made outside the command vocabulary does not reach that point, and
the component owning such a path SHALL advance the serial where it already
declares its caches stale.

#### Scenario: The two sides of an edit are distinguishable
- **WHEN** a value derived from the document is taken before a command is applied and again after it
- **THEN** the serial differs between the two, so a cache keyed on it cannot answer the second with the first

#### Scenario: Undo is covered without being named
- **WHEN** a document is changed and then undone
- **THEN** the serial advances for the undo as it did for the edit, because both apply a command

#### Scenario: A refused command changes nothing
- **WHEN** a command is refused, for instance against a protected layer
- **THEN** the serial does not advance

### Requirement: A layer's extent can be kept across an edit

An intersect is bounded by its layer's extent, and computing that extent walks
every visible item in the layer. A consumer that edits a layer repeatedly SHALL
be able to keep that extent across edits rather than recompute it per edit,
without the kept extent ever differing from a freshly computed one.

The kept form SHALL be exact rather than conservative. In particular the extent
SHALL shrink when the edit shrinks it: a form that can only be widened is not
the extent, and an extent that is too LARGE is merely slow while one built by
widening a stale union is too SMALL — which is under-invalidation, and renders
as stale geometry rather than as an error.

It SHALL remain cheap for the item being edited repeatedly even when that item
determines how far the extent reaches, because the item a host drags across a
form is typically a boolean operand large enough to extend past it. A form that
is cheap only for items lying strictly inside the extent does not satisfy this,
having no effect on the case it exists for.

Being told that an item changed SHALL NOT itself compute anything. A layer
holding no intersect never has its extent asked for, and such layers are the
common case; work done at the moment of the edit is therefore paid by layers
that never benefit from it.

The kept extent SHALL be abandoned for any change it cannot account for,
including adding, removing or reparenting an item, any change made to the layer
itself, and any mutation reaching the layer outside the command vocabulary.

Whether a given command is confined to a single item SHALL be decided in one
place, so that a component keeping an extent and a test checking one cannot
disagree about a command, and a command added later is classified rather than
assumed harmless.

#### Scenario: A drag walks the layer once
- **WHEN** one item is edited over many consecutive frames
- **THEN** the layer is walked on the first of them and not on the others, and the extent equals a freshly computed one on every frame

#### Scenario: The dragged item sticks out of the form
- **GIVEN** the edited item extends past the rest of the layer on some face
- **WHEN** it is dragged over many frames
- **THEN** the layer is still walked only once

#### Scenario: The edit makes the extent smaller
- **WHEN** the edited item is moved back inside the others, or made smaller
- **THEN** the extent shrinks to match a freshly computed one

#### Scenario: A layer nobody asks about is not walked
- **WHEN** a layer holding no intersect is edited over many frames
- **THEN** its extent is never computed

#### Scenario: A change the kept form cannot account for
- **WHEN** an item is added or removed, or the layer's own transform, mirror or radial setting changes
- **THEN** the next extent is computed by walking rather than kept

### Requirement: A chain of finite-support brushes is charged for what can meet

An item's declared Lipschitz bound SHALL be derived from the deformers that can
act together at one point, and SHALL NOT grow with the number of deformers that
cannot.

A brush with finite support is the identity outside its own region. Two such
brushes contribute a compounded factor only where both are non-identity for one
evaluation, which requires that a point inside the first can still be inside the
second when the second sees it. What carries it there is the travel of the links
BETWEEN them; the travel of the chain as a whole SHALL NOT be used, because it
grows with every brush added and so makes every pair look reachable on a
sufficiently worked model.

Reachability SHALL NOT be closed transitively for this purpose. Three brushes
where the first meets the second and the second meets the third, but the first
and third do not, have no point at which all three act — and a bound that
multiplies all three charges a compounding that cannot happen. Along a stroke
that is every brush on the model.

The result SHALL remain an upper bound: every deformer acting at a point
contains that point, so any set that acts together lies within the reach of each
of its members.

#### Scenario: A walked stroke does not compound along its length
- **WHEN** brushes are placed along a surface so that each overlaps only its neighbours
- **THEN** the bound does not grow in proportion to how many were placed

#### Scenario: Brushes on one spot still compound
- **WHEN** brushes are placed on top of one another
- **THEN** each additional one makes the bound larger, because they genuinely do stack

#### Scenario: The relaxed bound is still a bound
- **WHEN** the field of a chain of spread brushes is marched by the declared step
- **THEN** no step crosses the surface

### Requirement: An already-compiled tape can be baked into a volume
Sampling a layer's field into a volume is two things: compiling the layer into a tape in the layer's own frame, and sampling that tape. The second SHALL be reachable on its own, for a caller that holds a tape belonging to no layer — a PREFIX of a layer's edit list is exactly such a tape.

It SHALL be the same code the layer form runs: the same sampling, the same post-process, the same colour pass, the same measured steepness. A volume produced this way SHALL be one a consolidation could have produced, so that there is ONE definition of what a baked volume is rather than two that agree today.

The caller SHALL owe the two things a tape cannot state for itself, and the interface SHALL make both explicit rather than guess:

- THE FRAME. A layer bake compiles a local view — the layer visible and its own transform identity — because sampling the world-space field and then putting the result back under the layer applies the transform twice. A caller compiling its own tape owes the same convention.
- WHETHER COLOUR IS CARRIED. The compiler folds colour into instructions, so by the time a tape exists the question "can this produce more than one colour" can no longer be asked of it. The rule that answers it for a layer SHALL stay reachable, because it is the rule any caller has to apply to get the same bytes — and passing the wrong answer produces different BYTES, not a slower path.

The tape SHALL be borrowed rather than owned, and SHALL outlive the call. Refusals SHALL be the layer form's refusals — no resolution, nothing to sample — and a cancelled bake SHALL discard rather than return a partial volume.

#### Scenario: Baking a tape reproduces baking its layer
- **GIVEN** a layer, and the tape compiled from it under the frame convention above
- **WHEN** each is baked with the same parameters and the same colour answer
- **THEN** the two volumes serialize to the same bytes

#### Scenario: The colour answer is the caller's, and it changes the bytes
- **WHEN** a tape from a one-colour layer and a tape from a many-colour layer are each baked with the colour answer their layers give
- **THEN** each volume carries a colour channel exactly when its layer's bake does, and each matches its layer's bake byte for byte

#### Scenario: A tape bake refuses what a layer bake refuses
- **WHEN** a bake is attempted with no resolution, or on a tape with nothing to sample
- **THEN** it produces nothing
- **AND WHEN** a bake is cancelled through its token
- **THEN** it produces nothing rather than a partial volume
