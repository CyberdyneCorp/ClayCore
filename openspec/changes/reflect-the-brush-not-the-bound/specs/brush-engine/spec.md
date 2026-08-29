# brush-engine — under symmetry, reflect the brush, not the bound

Delta for `reflect-the-brush-not-the-bound`.

## MODIFIED Requirements

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
