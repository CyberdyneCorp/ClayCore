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

Consolidated output is therefore NOT byte-identical to what this build produced before. The bit-identity gate that guards consolidation SHALL be re-baselined deliberately, in the same change, with the reason recorded — a silent re-baseline of a gate that exists to catch silent change would be the worst possible way to ship this.

#### Scenario: A two-colour layer consolidates to a two-colour volume
- **WHEN** a layer holding a red item and a blue item is consolidated and the result is evaluated at points inside each
- **THEN** the reported colours are red and blue, not one colour for both

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
