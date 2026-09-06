## ADDED Requirements

### Requirement: Visible SDF layers fold under a per-layer operator

Each SDF layer SHALL describe how it combines with the accumulated field of the
visible SDF layers below it, using the SAME operators, blend profiles, blend
radii and rounding that an item uses. A layer boolean is the operation an item
boolean already is, so it SHALL NOT have its own vocabulary, its own evaluator or
its own kernel math.

**The first visible SDF layer SHALL initialise the accumulator and its own
operator SHALL NOT be applied.** Applying one against an empty field makes
`Subtract` and `Intersect` produce nothing at all, with no error, which is what
an artist who reorders their base layer to the top would otherwise see.

WHICH layer that is SHALL be decided from the document's visible SDF layer list
and from nothing else. It is not a property of what a particular compile
produced: a tape compiled for one region may hold no contribution at all from
the layers beneath a composed one, and a compiler that read first-ness from its
own accumulator would stop applying that layer's operator for that region alone
— a subtracting cutter rendering as material, an intersecting one no longer
cutting, in one brick and not its neighbour, with nothing reported. A layer that
is not the document's first SHALL therefore fold against the far field where its
accumulator is absent, which is what the item-level chain already does with an
item that opens one.

The fold SHALL remain symbolic. The field below a layer SHALL NOT be sampled into
a volume in order to combine with it: that would make an organisational act
destructive and would fix the document's resolution at whatever the fold chose.

A layer's own symmetry SHALL be resolved BEFORE it combines with what is beneath
it. Combining each mirrored or radial copy separately changes the result wherever
the blend is smooth, because a smooth combine does not associate.

A fold's reported extent SHALL cover the surface the fold can produce: the
layer's own extent dilated by the FOLD's support, taken from the same expression
the item-level combine uses, so a smooth or extended layer join cannot bulge past
the box the tape reports. A bound that is too small loses ray hits and drops
bricks from a plan, and both render as missing surface rather than as an error,
which is why this half is required rather than advisory.

Bounds are NOT required to be narrowed per operator, and this is a deliberate
limit rather than an omission. A subtract cannot create material outside its left
operand and an intersect is contained by the intersection, so both could report
less than the union — but the ITEM path unions for every operator too, and the
requirement above that a layer boolean and an item boolean express the same
document means narrowing one side alone would break it. Narrowing both changes
the meshing region of every document that already carries a subtract or a paint,
so it belongs to a change that can measure that. Until then a fold's extent is
conservative in the direction that cannot lose surface.

Exactness and the Lipschitz bound SHALL fold exactly as the item-level combine
folds them, so that a document expressing a shape as two layers and a document
expressing it as one layer of two items agree in distance, colour, bounds and
safe step.

Layer order and visibility SHALL therefore be GEOMETRIC. Hiding a subtractive
layer SHALL restore the geometry it was cutting, and reordering layers SHALL be
capable of changing the shape.

A layer whose kind cannot enter the tape SHALL REFUSE a composition rather than
store one that does nothing, so that a control a host offers is a control that
acts.

The region an edit reports as dirty SHALL cover every point the edit can change,
which under a fold is more than the edited layer's own extent in two ways. An
edit is carried up the stack through each fold it passes, so it SHALL be dilated
by the SUPPORT of that layer's own fold and of every fold above it — a combine is
pointwise in its operands, but a smooth or extended one moves its result up to
its own support away from where they moved, which is the dilation an enclosing
group already forces one level down. And a command that changes WHICH layers are
visible SDF layers — adding, removing, hiding, showing, or the remove-and-add
pair a reorder is — can move the first-visible rule onto the layer above, so it
SHALL also cover that layer's extent whenever that layer is composed. Both are
silent when missed: a brick outside the reported region keeps the values it has
and is stamped with the new revision, so it is never recomputed and never
reports anything.

EVERY ROUTE THAT ANSWERS "where can an edit reach in this document" SHALL
answer it the same way. The influence-bound QUERIES a host reads, the dirty
calls it makes, the region an applied command invalidates, and the reach a
GESTURE states once for a whole stroke SHALL all be the one expression: a
box in the edited layer's field, carried up by the folds above that layer. A
host computes its refill region from the query and hands it to the dirty call,
so a query that reports less than the command path dirties leaves that host
with stale geometry having asked for exactly what it was told, and the symptom
on its side is missing surface with nothing to point at. A gesture is the same
requirement in the one place a command bound is never derived: it states its
reach itself, so it SHALL take the same term, and a dab issued as a stroke SHALL
invalidate what the same dab issued as an edit invalidates.

Where a bound is computed from a LAYER alone it CANNOT answer this — the folds
above are a property of the stack — so such a function SHALL say so and name the
document-level form instead of being widened into a second answer.

A REGION-LIMITED compile SHALL answer, inside its band, what the whole-document
compile answers. A cull region is an optimisation and not a smaller document: an
item outside it can still steer the value inside it through the folds above that
item's layer. So the pad a culled compile dilates its region by SHALL include the
supports of the folds a layer's value passes through on its way to the document's
value — the same quantity the dirty region above carries, asked in the other
direction. SUMMED rather than maxed, because a stack of folds composes; charged
to the layers BENEATH each fold, because those hold the items a region would
otherwise drop; and not counting the first visible SDF layer's own composition,
which is never applied. Too wide keeps items a compile did not need and costs
tape; too narrow returns a field that never existed, per brick, with no error and
no visual tell beyond geometry that looks deliberate.

A document saved before layer composition existed SHALL load with every layer
unioning, and SHALL render exactly as it did.

#### Scenario: A layer cuts the layers below it
- **WHEN** a visible SDF layer is set to subtract and sits above another
- **THEN** the document's field is the lower layers with that layer's shape removed, and hiding it restores the uncut geometry exactly

#### Scenario: Two layers and one layer agree
- **WHEN** a shape is expressed as layer A with layer B subtracting, and as one layer holding A then B subtracting, under equivalent transforms
- **THEN** the two documents agree in distance, colour, bounds and safe-step scale over many sampled points

#### Scenario: The stack's order is part of the shape
- **WHEN** the same three layers are ordered A−B+C and A+C−B
- **THEN** the two produce different geometry, and each is stable across saves and reloads

#### Scenario: A region that holds none of the layers beneath still folds
- **WHEN** a tape is compiled for a region that no layer beneath a subtracting or intersecting layer reaches
- **THEN** that layer's operator is still applied, against the far field, and the region reads as the whole-document field reads there rather than as that layer alone

#### Scenario: An edit under a soft fold dirties what the fold moved
- **WHEN** an item is edited inside a layer that folds smoothly, or inside a layer beneath one
- **THEN** the dirty region covers every point whose band value changed, including those the fold's support carried outside the edited item's own reach

#### Scenario: The query a host dirties by is the region the command dirties
- **WHEN** a host reads a node's or a layer's influence bound across an edit under a smooth fold and dirties by the union
- **THEN** that box contains every point whose band value changed, and it is the same box the applied command and the corresponding mark-dirty call use

#### Scenario: A stroke invalidates what its dabs would have invalidated
- **WHEN** a stamp stroke, a surface drag or a surface magnify runs on a layer with a smooth fold above it
- **THEN** the one region the gesture invalidates carries the folds above that layer, so it covers what the same edits issued one at a time would have covered

#### Scenario: Hiding the bottom layer dirties the layer it promotes
- **WHEN** the bottom-most visible SDF layer is hidden, removed or reordered away, under a composed layer
- **THEN** the dirty region covers that composed layer's own extent, and a refill of bricks outside the hidden layer's box answers what a document built that way from scratch answers

#### Scenario: The first visible layer is not applied against nothing
- **WHEN** the first visible SDF layer is set to subtract or intersect
- **THEN** it initialises the accumulator instead, and the document shows that layer rather than an empty field

#### Scenario: An old document is unchanged
- **WHEN** a document saved before this feature is loaded
- **THEN** every layer unions and the field is bit-identical to what that document produced before

#### Scenario: A non-SDF layer refuses a composition
- **WHEN** a composition is set on a mesh or voxel layer
- **THEN** it is refused, rather than stored and ignored
