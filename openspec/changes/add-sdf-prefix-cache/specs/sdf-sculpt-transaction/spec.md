# sdf-sculpt-transaction — a gesture that materializes what it touches

Delta for `add-sdf-prefix-cache`.

`add-sdf-sculpt-transaction` gave Smooth a whole-layer working volume, sampled
once at pointer-down, and said in its own design why: a local patch needs a rule
for what it means where it meets the field it was cut from, and that rule is new
correctness surface. It also said what the follow-up was, and left `begin()`
O(model) until the rule was written down. It is now written down, and the two
requirements that described the eager shape are replaced here.

## RENAMED Requirements

- FROM: `### Requirement: A held gesture evaluates its source once`
- TO: `### Requirement: A held gesture pays for what the brush touches, not for the model`

## MODIFIED Requirements

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

## ADDED Requirements

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
