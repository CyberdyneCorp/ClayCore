# sdf-sculpt-transaction — a gesture the document does not see until it ends

Delta for `add-sdf-sculpt-transaction`. This capability is new.

It covers the lifetime a FIELD brush (Smooth, which bakes) and a DEFORMATION
brush (Move, which warps every item it reaches) need and an edit-list brush
does not. The edit-list brushes are unaffected and stay where they are: a dab
IS a persistent node, and there is nothing transient to hold.

## ADDED Requirements

### Requirement: A sculpt gesture can be held open without touching the document
The library SHALL provide a transaction over one SDF layer with four verbs — begin, update, commit and cancel. Between begin and commit the persistent document SHALL be untouched: no nodes added, removed or reparameterised, no deformer chain changed, and no undo entry pushed. A serialization taken while a gesture is open SHALL be byte-for-byte the one taken before it began.

Cancel SHALL be lossless and SHALL cost nothing to unwind, because nothing persistent was ever written. A cancelled or committed transaction SHALL be dead: it SHALL update nothing and commit nothing afterwards.

#### Scenario: An open gesture is invisible to the document
- **GIVEN** a serialization of a document taken before a gesture begins
- **WHEN** a transaction is opened on one of its layers and updated repeatedly
- **THEN** the document serializes to exactly the same bytes

#### Scenario: No undo step exists until the commit
- **WHEN** a transaction is opened and updated any number of times
- **THEN** the undo stack has gained nothing, and the commit adds exactly one step

#### Scenario: A cancel leaves nothing behind
- **WHEN** an updated transaction is cancelled
- **THEN** the document is byte-for-byte what it was, and the transaction is no longer live

### Requirement: A held gesture evaluates its source once
A transaction SHALL do the work that scales with the MODEL exactly once, at begin, and SHALL NOT repeat it per update or at commit. A Smooth gesture SHALL sample its layer's field once and hold the result; a Move gesture SHALL traverse the layer's edit list once to decide which items the drag reaches and where its anchor lands in each of their frames.

Per-update work SHALL therefore be proportional to what the gesture touches rather than to what the document holds. For a Move that means the items the drag reaches, whatever else the layer contains, and the transaction SHALL expose the counters that state this — what preparation walked, and what the last update visited — so it can be asserted as a number rather than as a duration.

#### Scenario: A Smooth gesture bakes once
- **GIVEN** a transaction counting evaluations of its source layer
- **WHEN** it is opened, updated several times and committed
- **THEN** the evaluation count after begin is the count after the last update and after the commit

#### Scenario: A Move frame does not notice unrelated model
- **GIVEN** two layers holding the same reachable items, one of them with thousands of items far out of the drag's reach
- **WHEN** a drag is prepared on each and then updated
- **THEN** preparation visits every node of each layer, and each update visits only the items the drag moves — the same number for both

### Requirement: An update reports the region it changed
An update SHALL report what it changed: a world-space bound, a count of the units of storage it touched, and whether anything actually moved. A host SHALL be able to invalidate a region from that report rather than invalidate the model.

The bound and the count SHALL be conservative and geometric — they describe where the gesture acted, not the samples whose values happened to differ — so that the same brush over the same lattice reports the same numbers however much unrelated model surrounds it. Whether anything moved SHALL be reported separately, because a dab whose weight came out zero everywhere still acted somewhere and a host still wants to know it has nothing to redraw.

#### Scenario: A dab reports where it landed
- **WHEN** a Smooth transaction is updated with a region-limited dab
- **THEN** it reports a non-empty bound, a non-zero touched count, and that something changed

#### Scenario: A drag's reported region covers where the surface was and where it went
- **WHEN** a Move transaction is updated with a displacement
- **THEN** the reported bound contains both the drag's anchor ball and the ball it has been dragged to

### Requirement: What a gesture previewed is what its commit installs
A host SHALL be able to draw a gesture's preview through the paths it already has, and a commit SHALL install exactly what was previewed rather than something computed a second time.

A Smooth transaction SHALL expose its working volume, and its commit SHALL install that volume as the layer's single item WITHOUT sampling the layer again. A Move transaction SHALL expose a preview as ordinary scene content — a layer whose affected items carry the drag — so it compiles, draws and picks like any other layer.

A live sequence of updates SHALL equal the same sequence applied through the standalone operations, sample for sample. A preview that is merely close to what its commit produces is a preview of something else.

#### Scenario: The live sequence equals the standalone sequence
- **WHEN** a Smooth transaction is updated with a sequence of dabs
- **THEN** its working volume is byte-identical to the same dabs applied one after another through the standalone relax

#### Scenario: The committed item is the previewed volume
- **WHEN** a Smooth transaction is committed
- **THEN** the layer's single item holds bytes identical to the volume the transaction was previewing

#### Scenario: The Move preview carries the drag and the document does not
- **WHEN** a Move transaction is updated
- **THEN** every affected item in the preview carries one warp from the drag, and the same items in the document carry none

### Requirement: A committed gesture is one undo step
A commit SHALL record everything it does as ONE undo step, however many commands it issues and however many items a drag reached. Undoing it SHALL restore the document to what it was before the gesture — absorbed items back with their ids, parameters, colours and deformers; warped chains back to what they were — and redoing it SHALL restore what the commit produced.

Anything the gesture's own policy triggers after the stroke SHALL be inside that same step, because the artist did one thing and must undo one thing.

#### Scenario: A Move drag across many items is one step
- **WHEN** a drag reaching several items is committed
- **THEN** the undo stack has gained exactly one entry, and one undo restores every affected chain

#### Scenario: Undo and redo of a Smooth are exact
- **WHEN** a committed Smooth is undone and redone
- **THEN** the document serializes to what it was before the gesture, and then to what the commit produced

### Requirement: A commit refuses a source that moved underneath it
A transaction SHALL stamp the identity of its source layer at begin and re-check it at commit. When the layer has been edited, removed or protected in the meantime, the commit SHALL FAIL and change nothing — no partial step, no overwritten edit.

A preview computed from a document that no longer exists SHALL NOT be allowed to overwrite the edit that replaced it. The stamp SHALL be derived from the layer's CONTENT rather than from a counter a mutation could forget to bump, and SHALL therefore move for any edit that changes what the layer evaluates to — including one made through a sibling instance layer, which shares the same edit list.

#### Scenario: An external edit is not overwritten
- **GIVEN** an open transaction on a layer
- **WHEN** another edit is applied to that layer and the transaction is then committed
- **THEN** the commit fails, the external edit is intact, and the undo stack has gained nothing

#### Scenario: The stamp reads the content, not the pointer
- **WHEN** an edit is made through one of two layers sharing an edit list
- **THEN** the stamp of both layers has moved

### Requirement: A transaction refuses what it cannot own
Beginning a transaction SHALL fail, producing nothing, when the layer does not exist, is not an SDF layer, has no edit list, or is protected from edits. A Smooth transaction SHALL additionally refuse a sampling resolution of zero — a document has no intrinsic one to derive — and a layer whose field could not be sampled, including when the sampling was cancelled. A Move transaction SHALL additionally refuse a non-positive radius, which does not describe a drag.

A drag that reaches NOTHING SHALL be a valid transaction with no affected items rather than a refusal: the artist pressed on empty space, which is not an error, and its commit SHALL record nothing.

#### Scenario: A protected layer cannot be sculpted
- **WHEN** a transaction is begun on a locked layer, a ghosted layer, a non-SDF layer or an unknown layer
- **THEN** no transaction is produced and the document is untouched

#### Scenario: A drag on empty space commits nothing
- **WHEN** a drag whose radius reaches no item is begun and committed
- **THEN** it reports no affected items and the undo stack has gained nothing

### Requirement: A Move update takes the total displacement, not an increment
A Move update SHALL take the displacement measured from the gesture's anchor, and a sequence of updates SHALL end at exactly what a single fresh drag of the final displacement produces.

Each frame SHALL be resolved from the state captured at begin — the item's prepared frame and its PRE-STROKE deformer chain, held by value — and never from the previous frame's result. A composition of increments is a composition of warps each authored against a different intermediate surface, which is not the drag the artist made; and because the pull is deliberately less than the displacement asked for, such a composition does not converge to it either.

The commit SHALL rebuild the final chains from that same captured state rather than trust the preview, so a commit is correct even if the host never called update and the two can never be computed by different code.

#### Scenario: Three updates equal one drag
- **WHEN** a transaction is updated with a growing displacement and committed
- **THEN** the field it produces is the field of a single drag of the final displacement

#### Scenario: One warp per item, not one per frame
- **WHEN** a drag is updated many times
- **THEN** every affected item carries exactly one warp from that drag

### Requirement: Collapsing history to keep the marcher affordable is the session's decision
The library SHALL provide a policy by which a session states when repeated sculpting has degraded a layer far enough to be worth collapsing, and SHALL evaluate it after a committed gesture. The criteria SHALL be the ones the field report already measures separately — the safe step scale, the longest deformer chain and the item count — kept apart rather than aggregated, because they decay for different reasons and an aggregate cannot say which to cure. A zeroed criterion SHALL be disabled, so a value-initialised policy authorises nothing and measures nothing.

Being over budget SHALL be a REPORT by default. Collapsing SHALL happen only when the session has explicitly authorised it, and SHALL then run inside the gesture's own undo step. The engine SHALL NOT decide on its own to discard an artist's parametric history — the measuring entry points keep that contract unchanged, and this adds only somewhere for a host to say when it is acceptable.

A layer that is already a single volume SHALL NOT be collapsed again: resampling samples into samples changes nothing about what the layer costs and lowers the bound the marcher depends on.

Where an authorised collapse resamples SHALL be the policy's own resolution when it states one, and otherwise the resolution the gesture is already working at.

#### Scenario: An empty policy authorises nothing
- **WHEN** a report is judged against a value-initialised policy
- **THEN** it is not over budget

#### Scenario: Over budget without authorisation changes nothing
- **WHEN** a gesture commits on a layer that exceeds an unauthorised policy
- **THEN** the budget reports over budget, reports nothing consolidated, and the layer's items are exactly what the stroke left

#### Scenario: An authorised collapse is part of the same undo step
- **WHEN** a gesture commits on a layer that exceeds an authorised policy
- **THEN** the layer is collapsed, the undo stack has gained exactly one entry for the whole gesture, and one undo restores the layer as it was before the stroke

#### Scenario: A layer that is already one volume is not baked again
- **GIVEN** a Smooth commit, which leaves the layer as a single volume item
- **WHEN** an authorised policy finds that layer over budget
- **THEN** nothing is consolidated and the installed volume is byte-identical to what the stroke previewed
