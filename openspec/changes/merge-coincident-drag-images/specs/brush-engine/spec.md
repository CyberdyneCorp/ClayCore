## MODIFIED Requirements

### Requirement: A world drag resolves into a field-level move
The library SHALL resolve a world-space centre, radius and displacement into the per-item warps that reproduce that drag on the ASSEMBLED surface of a layer, rather than on one item of it. The result SHALL be returned rather than applied, so a host can preview a drag before committing it and decide which commands carry it — the same rule the stroke engine's node consumer follows.

Each warp SHALL be expressed in its item's OWN frame, mapping the world centre, radius and displacement through that item's world transform — which is the layer's transform composed with the item's, because a group's transform does not reach its children in this scene model. The resolver SHALL agree with the evaluator on that point rather than accumulating a chain the evaluator does not.

Each warp SHALL be marked for the FRONT of its item's chain, because a warp appended behind an existing deformer has its region weight evaluated at a point that deformer already moved.

Under the layer's symmetry the drag SHALL be stated as its IMAGES — the ball itself, one reflection per set mirror axis and one rotation per radial copy, additively and never as products, which is exactly the set of copies the compiler emits of an item — and each item SHALL be tested on its OWN influence bound, without the reflected or rotated copies, against each image. An image that reaches an item yields one grab at that image's centre with that image's displacement, so the reflected ball grabs the items whose reflections sit under the ball; an item no image reaches SHALL receive no warp at all, since a deformer with finite support, outside its own support, is a no-op that still costs a tape record on every evaluation. An item that does not participate in the symmetry SHALL see the drag alone. A node's grabs SHALL be ordered by their values, never by which image produced them, and a warp SHALL name every image the item can see so that a continuing gesture can recognise all of its earlier frames.

Images whose centres COINCIDE — a drag centred on a mirror plane, or on a radial axis — SHALL be resolved as one brush rather than one grab each, since a grab composed with itself is a second brush and not the same one. Coincidence SHALL be decided per drag from the world centres, within a tolerance that absorbs the rounding a reflection picks up through a placed layer transform and no wider. A group of coincident images SHALL contribute the mean of their displacements as one grab and, for each image, one grab for what it adds beyond that mean, dropping components below a relative tolerance: a pull along the plane from on it SHALL then be exactly the unmirrored drag's grab, a pull straight across it SHALL keep its two opposite grabs, and an oblique pull SHALL apply its along-plane part once. Images that do not coincide SHALL keep one grab each, unchanged. The step this leaves between a centre on the plane and one just off it SHALL be documented, because the continuous rule is not a composition of grabs.

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
- **WHEN** a drag's ball and its reflection, distinct balls, both reach an item straddling the plane
- **THEN** the item takes one warp carrying one grab per image, the two pulls compose as two brushes would, and the grabs are ordered by value so the +x drag and its mirror image produce the same field whether or not the item is itself plane-symmetric

#### Scenario: A drag on the plane pulling along it is one brush
- **GIVEN** a unit sphere on an x-mirrored layer
- **WHEN** a drag from (0, 1, 0) by (0, 0.25, 0) at radius 0.35 and linear ease is applied, through the held call or the live transaction
- **THEN** the straddling item takes the unmirrored drag's single grab and the surface rises as far as it does with no mirror, where one grab per image lifted it 1.58x as far

#### Scenario: A drag on the plane pulling across it still pinches
- **WHEN** a drag centred on the plane pulls straight across it
- **THEN** the coincident images' mean pull is zero and the straddler keeps its two opposite grabs, narrowing it symmetrically

#### Scenario: An oblique drag on the plane shares its along-plane pull once
- **WHEN** a drag centred on the plane pulls obliquely
- **THEN** the straddler takes one grab carrying the along-plane part and two opposite grabs carrying the across-plane parts

#### Scenario: Coincidence tolerates a placed layer and nothing wider
- **WHEN** a drag on the plane of a translated, rotated and scaled layer is resolved, and separately a drag 0.001 off the plane of an identity layer
- **THEN** the first straddler takes one grab despite the reflection's rounding, and the second takes two because its images are distinct balls

#### Scenario: A drag on a radial axis along it is one brush
- **WHEN** a layer carries a radial count and a drag centred on the axis pulls along it
- **THEN** the item under it takes one grab rather than one per copy

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

### Requirement: A world magnify resolves into a field-level radial scale
A magnify stated in WORLD space — a centre, a radius and a SIGNED strength — SHALL resolve into one `magnify` deformer per item the region reaches, each already in that item's own frame, so that the layer's ASSEMBLED surface swells or gathers rather than one item's share of it.

`magnify` is per item and applied to that item's local point, exactly as `grab` is, so a magnify put on one item of a smooth-unioned form scales that item's field and leaves the others where they were. This is the hazard `move_brush` exists for, and it applies to the radial scale verbatim.

A POSITIVE strength SHALL swell the surface away from the centre and a NEGATIVE one gather it toward. One signed parameter covers Magnify and Pinch, which are one deformation.

The strength SHALL cross the layer's symmetry images unchanged: a reflection or a rotation of a radial scale is a radial scale of equal strength, unlike a drag's displacement, which has to be mapped per image.

Images whose balls COINCIDE — a gesture centred on a mirror plane or on a radial axis — SHALL resolve to ONE magnify, reaching the item when any image of the group does: with the same centre and the same strength they are the same deformation, and composing it with itself would scale twice (#663).

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

#### Scenario: A magnify on the mirror plane is one magnify
- **GIVEN** a unit sphere on a layer mirrored about x with a hard seam
- **WHEN** a magnify centred at (0, 1, 0) at radius 0.35 is resolved and applied
- **THEN** the straddling item takes one magnify and the surface off the centre moves as far as with no mirror, where one magnify per image moved it twice as far
