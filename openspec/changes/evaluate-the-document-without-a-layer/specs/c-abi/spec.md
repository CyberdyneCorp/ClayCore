# c-abi

## ADDED Requirements

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
- **WHEN** a consumer takes the minimum of an excluded evaluation and the excluded layer's own evaluation at the same points
- **THEN** the result equals the whole-document evaluation at those points

#### Scenario: A stale layer id is refused
- **WHEN** a consumer excludes a layer identifier the document does not hold
- **THEN** the call returns a not-found error and writes no distances

#### Scenario: Excluding a hidden layer is not an error
- **WHEN** a consumer excludes a layer that is hidden or carries no SDF content
- **THEN** the call succeeds and answers what the visible SDF layers evaluate to

#### Scenario: A brick refill without one layer
- **WHEN** a consumer evaluates brick requests excluding one layer
- **THEN** brick i occupies the same fixed slot it occupies in the whole-document form, holding what that brick would hold in a document without that layer
