# sdf-sculpt-transaction Specification

## Purpose

A sculpt gesture — a Smooth held down, a Move dragged — is many pointer events
that must feel like one edit and become one undo step. This capability is the
transaction that holds such a gesture open: the document does not move until the
commit, the preview a host draws is exactly what the commit installs, and a
cancel is a discard that leaves the document byte-identical.

It exists so that per-event work is proportional to what the brush TOUCHES
rather than to what the document holds, and so that a host can draw a frame from
a gesture in flight without asking the document a question the threading rules
forbid.

## Requirements

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

### Requirement: A held gesture pays for what the brush touches, not for the model
A transaction SHALL NOT do work proportional to the MODEL per pointer event, and SHALL NOT do it at begin either.

Beginning a gesture SHALL NOT evaluate the layer's field at all. A Smooth gesture SHALL begin with a working field that has the layer's lattice and no stored samples, and SHALL bring in the storage a dab reads AS THE DAB ASKS FOR IT: the region the dab rewrites, plus what its stencil reads from outside that region, and nothing else. Storage already brought in SHALL be reused rather than refilled, because refilling it would discard the edits earlier dabs made to it.

The region a dab brings in SHALL be derived from the operator's own stencil rather than estimated. It SHALL account for the operator silently widening a falloff narrower than its kernel, for the stencil's reach in cells, and for a rewrite writing whole units of storage that merely TOUCH the brush's ball — so a written sample can lie a whole unit DIAGONAL beyond the ball. A dependency region that is short does not read a wrong value: it reads NOTHING, the operator renormalizes over the taps that exist, and the result is a seam at a storage boundary that is invisible except as a measurement.

A Move gesture SHALL traverse the layer's edit list once, at begin, to decide which items the drag reaches and where its anchor lands in each of their frames.

The transaction SHALL expose the counters that make this a number rather than a duration — what has been brought in, what was reused, what preparation walked, and what the last update visited — so that the claim holds on a loaded machine as firmly as on an idle one.

#### Scenario: Beginning a Smooth gesture evaluates nothing
- **WHEN** a Smooth transaction is opened on a layer
- **THEN** it has brought in no storage, run no update, and its working field stores no samples at all

#### Scenario: A dab brings in what it reads, and a repeat brings in nothing
- **WHEN** a dab is applied, and then the same dab is applied again
- **THEN** the first brings in storage and the second brings in none, reusing what is there

#### Scenario: A dab elsewhere brings in its own region and no more
- **WHEN** a second dab is applied well away from the first
- **THEN** what it brings in is its own region, not the model

#### Scenario: Unrelated model does not change what a dab costs
- **GIVEN** two layers alike where the brush is, one of them with hundreds of items far out of its reach
- **WHEN** the same dab is applied to each
- **THEN** each brings in exactly the same amount of storage

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
A host SHALL be able to draw a gesture's preview through the paths it already has, and a commit SHALL install what was previewed rather than re-running the gesture.

A Smooth transaction SHALL expose its working field, and its commit SHALL assemble the layer's final volume ONCE, on the same lattice, from the same source the dabs drew from, and overlay exactly the samples the dabs changed. It SHALL NOT re-run the brush, and it SHALL NOT re-evaluate what a dab already paid for inside the edited region. A Move transaction SHALL expose a preview as ordinary scene content — a layer whose affected items carry the drag — so it compiles, draws and picks like any other layer.

A working field is LOCAL and a layer is not, so the commit is an assembly rather than an install, and two consequences follow that SHALL be stated rather than asserted away:

- The working field SHALL be the layer's samples relaxed, so it SHALL agree with a whole-layer application of the same operations to the same lattice, **within the band**, to a small fraction of a cell. Past the band a lazily filled field and a fully sampled one hold different bounds on purpose, because bringing storage in is what records that it was filled.
- The committed volume SHALL NOT be required to be byte-identical to what a whole-layer gesture would have installed. That path relaxed a post-processed sampling; this post-processes a relaxed one. Both are sound fields and neither approximates the other, so exact parity is not reachable for a local field — the other path's starting point was globally post-processed. The distance between them SHALL be MEASURED and bounded on the surface an artist sees, and it SHALL be a fraction of a cell rather than a feature.

#### Scenario: The lazy working field equals a whole-layer application in the band
- **WHEN** a sequence of dabs is applied through a transaction, and the same sequence is applied to a whole-layer sampling of the same lattice
- **THEN** every sample in the band that both hold agrees to a small fraction of a cell

#### Scenario: The distance from the whole-layer commit is measured, not assumed
- **WHEN** one dab is committed through a transaction, and the same dab is applied through a whole-layer sampling and post-process
- **THEN** the two fields differ on the surface by a fraction of a cell, and that distance is asserted as a bound

#### Scenario: A commit assembles once and does not re-run the brush
- **WHEN** a Smooth transaction is committed
- **THEN** the layer's single item holds the assembled volume, and the brush was not applied a second time

#### Scenario: The Move preview carries the drag and the document does not
- **WHEN** a Move transaction is updated
- **THEN** every affected item in the preview carries one warp from the drag, and the same items in the document carry none

#### Scenario: The live sequence equals the standalone sequence
- **WHEN** a Smooth transaction is updated with a sequence of dabs
- **THEN** its working volume is byte-identical to the same dabs applied one after another through the standalone relax

#### Scenario: The committed item is the previewed volume
- **WHEN** a Smooth transaction is committed
- **THEN** the layer's single item holds bytes identical to the volume the transaction was previewing

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

### Requirement: A gesture that changed nothing installs nothing
A commit SHALL install nothing at all when no update moved anything: no volume, no undo entry, and no policy-triggered collapse. The layer SHALL keep every item it had, with its parameters intact.

A pointer-down followed by a pointer-up — with no dab between them, or with dabs whose strength was zero, or with a mask that froze everything the brush covered — is a gesture the artist did not make. Installing a sampled volume for it would replace an editable edit list with samples on the strength of a gesture that had no effect, and a collapse triggered by it would spend parametric history no stroke earned. A no-op SHALL NOT be a way to lose history.

The commit SHALL still report success, because nothing failed, and SHALL still end the transaction.

#### Scenario: Pointer-down and pointer-up alone change nothing
- **WHEN** a Smooth transaction is opened and committed with no update in between
- **THEN** the commit succeeds, the document serializes to exactly what it did before, and the undo stack has gained nothing

#### Scenario: A dab that moved nothing installs nothing
- **WHEN** a Smooth transaction is updated with dabs that move no sample, and then committed
- **THEN** the layer still holds its original items and no volume was installed

### Requirement: A host can take a gesture's preview incrementally
A transaction holding a working field SHALL be able to report WHICH units of its storage hold bytes a consumer has not seen — the ones an update brought in, and the ones an update's operator actually moved — so that a host draws a gesture by patching what changed rather than by copying the working field per frame.

The record SHALL ACCUMULATE until it is taken, so a host that skips a frame loses nothing, and SHALL be deduplicated by unit of storage, because an update that brings a unit in and then writes it produces the same coordinate twice by construction. Taking it SHALL hand it over and clear it; looking SHALL NOT clear it, so a host may ask every frame whether it is worth acting.

A generation SHALL be reported that moves when the preview MOVES and at no other time — not on an update that changed nothing, and not on taking. It names the state a consumer now holds, which is what lets a host tell a duplicate read from a skipped frame and abandon work it began against an older one.

The whole-field snapshot SHALL remain available, because a host joining mid-gesture or rebuilding a lost preview needs it and a simple host must not have to implement patching to draw anything at all.

#### Scenario: A dab reports the storage it changed
- **WHEN** a Smooth transaction is updated
- **THEN** the record names the storage the dab brought in and the storage its operator moved, and the generation has advanced

#### Scenario: The record accumulates and deduplicates
- **WHEN** the same dab is applied twice without the record being taken
- **THEN** the record names those units once, and the generation has advanced twice

#### Scenario: Taking clears the record and not the generation
- **WHEN** the record is taken
- **THEN** nothing is waiting afterwards, and the generation still names what the caller was handed

#### Scenario: An update that changed nothing does not advance the generation
- **WHEN** an update moves no sample and brings in no storage
- **THEN** the generation is where it was

### Requirement: A gesture may be accelerated by a cached prefix without depending on one
A transaction MAY be given a cache of sampled prefixes for the layer it is opened on, and when one holds a usable entry the work each dab pays SHALL be the suffix's rather than the whole history's.

It SHALL NOT depend on one. Opening a transaction SHALL NOT BUILD an entry — a build at the start of a gesture is the whole-layer cost this arrangement exists to remove — and with no cache, an empty cache or a declining policy, every fill SHALL be the full walk: slower, and the same answer.

The gesture's own sampling numbers SHALL be imposed on the cache policy it passes down, so that a caller cannot ask for a prefix at a resolution the gesture is not working at. A seed read off a different lattice is an interpolation rather than a stored sample, and sharing one lattice is the reason the acceleration is exact where it applies.

#### Scenario: A gesture with no cache is correct
- **WHEN** a Smooth transaction is opened with no cache and dabs are applied and committed
- **THEN** the result is what it is with a cache, and nothing failed for the want of one

#### Scenario: Opening a gesture builds no cache entry
- **WHEN** a Smooth transaction is opened against an empty cache whose policy would admit a prefix
- **THEN** no prefix was built and the cache's build count is unchanged
