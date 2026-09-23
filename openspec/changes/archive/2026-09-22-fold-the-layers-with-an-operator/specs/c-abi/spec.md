## MODIFIED Requirements

### Requirement: A host can evaluate the document without one layer

The API SHALL expose point evaluation, gradient evaluation and brick-request
evaluation over **every visible SDF layer except one named layer**, so a host
previewing a single layer through a transaction can draw the rest of the
document beside it.

The excluded forms SHALL answer exactly what the whole-document forms answer for
a document from which that layer had been removed. Visible SDF layers compose by
hard union, so a caller may take the minimum of an excluded evaluation and its
own preview and obtain the field the whole document would have — this is exact
composition, not an approximation, and the API SHALL say so where it is offered.

**THAT IDENTITY IS A PROPERTY OF THE UNION AND DOES NOT SURVIVE A LAYER
COMPOSITION.** Once any applied layer composition is not a hard Add, no combine
of the two parts reconstructs the document — removing a layer from the middle of
a fold changes what every layer above it folds ONTO — so the excluded forms SHALL
REFUSE such a document, naming the layer that composes, rather than answering
something that no longer composes back. The refusal SHALL name the alternative
where one exists, and it does exist for the case a live preview is usually in: a
preview of the LAST visible SDF layer composes exactly with the layers-below form
plus that layer's own composition. Excluding from the MIDDLE of a fold has no
repair, and the API SHALL say that too, so a host does not go looking for one.

The excluded forms SHALL take the same arguments, honour the same backend
selection, and observe the same count ceilings as the whole-document forms they
mirror. Brick-request evaluation SHALL fill the same fixed per-brick slots at
the same stride.

Naming an excluded layer the document does not hold SHALL return a not-found
error. It SHALL NOT be read as "exclude nothing": a host whose layer id went
stale would otherwise be handed the whole document, and would draw the layer it
meant to exclude on top of the preview it drew itself.

Naming a layer that is hidden, or that carries no SDF content, SHALL succeed and
answer what the visible SDF layers evaluate to — such a layer contributes
nothing to the union, so excluding it is already a no-op and refusing it would
make a host branch on state it has no reason to track.

These entry points SHALL NOT modify the document. In particular a host SHALL NOT
need to toggle layer visibility to obtain this result, because visibility is an
edit and an edit taken during a transaction is one the transaction's commit
refuses.

#### Scenario: The rest of the document, during a gesture
- **WHEN** a consumer opens a sculpt transaction on one layer of a multi-layer document and evaluates the document excluding that layer
- **THEN** it receives the field of the other visible SDF layers, the document is unchanged, no undo entry is recorded, and the transaction's commit still succeeds

#### Scenario: The excluded evaluation composes exactly
- **WHEN** a consumer takes the minimum of an excluded evaluation and the excluded layer's own evaluation at the same points, in a document where every visible SDF layer unions
- **THEN** the result equals the whole-document evaluation at those points

#### Scenario: A composed document is refused rather than answered
- **WHEN** a consumer evaluates points, gradients or brick requests excluding a layer, in a document where an applied layer composition is not a hard union
- **THEN** the call is refused with an invalid-argument error naming the layer that composes, and the documentation states what to use instead

#### Scenario: A stale layer id is refused
- **WHEN** a consumer excludes a layer identifier the document does not hold
- **THEN** the call returns a not-found error and writes no distances

#### Scenario: Excluding a hidden layer is not an error
- **WHEN** a consumer excludes a layer that is hidden or carries no SDF content
- **THEN** the call succeeds and answers what the visible SDF layers evaluate to

#### Scenario: A brick refill without one layer
- **WHEN** a consumer evaluates brick requests excluding one layer
- **THEN** brick i occupies the same fixed slot it occupies in the whole-document form, holding what that brick would hold in a document without that layer

## ADDED Requirements

### Requirement: A host can evaluate the layers below one

The API SHALL expose brick-request evaluation over **every visible SDF layer
below one named layer**, folded as the document folds them, so that a host
previewing the layer under the brush can still draw the rest of a document whose
layers compose. It SHALL take the same arguments, the same backend selection and
the same count ceilings as the whole-document form, and SHALL fill the same fixed
per-brick slots at the same stride.

What it answers SHALL be the accumulator the whole-document walk holds when it
reaches the named layer. So the layers beneath SHALL be folded with their OWN
compositions rather than unioned, and combining this half with the named layer's
own value under that layer's composition — the op, blend profile, blend radius
and rounding the composition reader reports for it — SHALL reproduce the
whole-document field over sampled points, in distance and in colour.

IT SHALL REFUSE ONLY WHEN THE NAMED LAYER IS NOT THE LAST VISIBLE SDF LAYER. A
visible SDF layer above it is in the document and in neither half, so no combine
of the two halves is the document; the layers BENEATH place no condition on the
call at all. Hidden layers, and mesh or voxel layers, above the named one SHALL
NOT block it: they are not in the fold, so a split beneath them is still the
whole document.

THAT REFUSAL SHALL REPORT BOTH THE LOWEST BLOCKING LAYER AND HOW MANY BLOCK IT.
The id names the row to act on first; the count is how many visible SDF layers
sit above the named one altogether, and it is what lets a host write the sentence
once rather than repeat it after each hide. A refusal that reported only the id
would be wrong by omission on a stack with two field layers above the target: the
sculptor acts on the one named, asks again, and is refused again naming the next.

The position restriction SHALL NOT be read as a narrowing of the excluding form,
and the API SHALL say so where it states the refusal. The excluding form refuses
on the DOCUMENT rather than on a position, so for a document where every applied
composition is a hard Add it keeps working at any stack position exactly as it
did before this change.

A layer with nothing beneath it SHALL be answered rather than refused, with the
far field, and the API SHALL state that the named layer's own composition is not
applied there — the first visible SDF layer initialises the accumulator — so a
host that composes unconditionally is told before it subtracts its preview from
nothing.

It SHALL store no resume seed and read none, for the reason the other scoped
forms do not: a value computed for part of the document is not a seed for the
document, and one stored would be resumed later as though it were the whole
field, which is a wrong answer with nothing in it to say so.

#### Scenario: The two routes agree
- **WHEN** a host evaluates the layers below the top layer, evaluates that layer alone, and combines the two under that layer's own composition, in a document whose lower layers compose and whose top layer composes
- **THEN** the result equals the whole-document brick refill at every sample, in distance and in colour

#### Scenario: The layers beneath may compose
- **WHEN** the layers below the named one carry subtracting and smoothly-blending compositions
- **THEN** the call is not refused, and the half it answers reflects those compositions rather than a union of the same layers

#### Scenario: A layer that is not the topmost is refused
- **WHEN** a host asks for the layers below a layer that has a visible SDF layer above it
- **THEN** the call is refused with an invalid-argument error, and the refusal reports the id of the LOWEST visible SDF layer above the named one

#### Scenario: The refusal says how many layers block it
- **WHEN** more than one visible SDF layer sits above the named layer
- **THEN** the refusal reports the lowest of them AND the total number above the named layer, so a host is not told to act on one row when several block the call

#### Scenario: What is above but not in the fold does not block it
- **WHEN** the layers above the named one are hidden SDF layers, mesh layers or voxel layers
- **THEN** the call succeeds

### Requirement: A refusal that has computed which layer is responsible SHALL return its id

Where the API refuses a call because of a particular layer OTHER than one the
caller named, it SHALL hand that layer's id back — through an out-parameter where
it has one and by naming it in the error detail otherwise — rather than reporting
only that it refused. The refusal has already walked the stack to decide; a host
that is told only "no" walks it again to say which row a person should act on,
and the difference between the two is a message that names an action and one that
names a wall.

This SHALL be held by a test that walks every such refusal, so that a refusal
added later without its id fails rather than being noticed.

#### Scenario: Every refusal that knows an id gives it
- **WHEN** a composition is set on a non-SDF layer, a document carrying a composition is asked whether it can be written at an older format minor, and the layers below a layer that is not the topmost visible SDF one are asked for
- **THEN** each call returns its error code and a non-zero id naming the layer actually responsible
