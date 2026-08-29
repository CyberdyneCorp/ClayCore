# sdf-prefix-cache — the history an artist has already sculpted, sampled once

Delta for `add-sdf-prefix-cache`. This capability is new.

It covers an EPHEMERAL, DROPPABLE field cache for an old and stable prefix of a
layer's edit list: what it may stand in for, what it may never stand in for,
when it must be considered stale, and the rule that its presence changes cost
and never output.

It is the split consolidation makes, without the loss. Consolidation samples a
layer's field into a volume and DISCARDS the nodes, which is why it costs an
artist their parametric history and why the library never does it unasked. This
samples the old roots and KEEPS them: the document is untouched, every item
stays editable, and the cache is derived state a host may throw away at any
moment.

## ADDED Requirements

### Requirement: A cached prefix changes what evaluation costs and never what it produces
The library SHALL be able to hold a sampled stand-in for a stable PREFIX of a layer's edit list and evaluate the remaining suffix onto it, and the answer SHALL be the answer a full walk of the whole layer gives.

Deleting every cached entry SHALL be semantically equivalent to flushing a processor cache: the same output, obtained more slowly. No caller SHALL be required to hold a cache for a correct answer, no entry point SHALL fail because a cache is absent, empty or declined, and every accelerated path SHALL have a correct slow path beside it that is taken whenever the fast one cannot be shown to apply.

The cache SHALL be derived state. It SHALL NOT be part of the document, SHALL NOT be serialized, SHALL NOT appear in undo, and SHALL NOT change what the document evaluates to.

#### Scenario: An accelerated field equals the full walk
- **GIVEN** a worked layer whose old roots are cached as a sampled prefix
- **WHEN** the layer's field is asked for many points scattered across it
- **THEN** every answer is the one a full walk of the whole layer gives

#### Scenario: With no cache at all the answers are unchanged
- **WHEN** the same points are asked of a source opened with no cache, and of one whose policy declines to cache anything
- **THEN** both give the full walk's answers, and the accelerated source gave them too

#### Scenario: The document does not notice
- **WHEN** a prefix is cached, used, invalidated and rebuilt
- **THEN** the document serializes to exactly the same bytes throughout, and every item of the cached prefix is still present and editable

### Requirement: A sampled prefix may only stand in where it stores every sample
A sparse sampled volume answers two different questions. Where it stores samples it interpolates them. Where it does not, it reports a CONSERVATIVE FAR BOUND — a value chosen so that a marcher stepping by it cannot cross the surface, and deliberately not the distance the field had there.

A suffix folded onto a far bound is therefore not the field: a blend against a number that was never a distance is wrong by many cells, not by rounding, and the error is categorical rather than a tolerance to be tuned — it barely moves with cell size or with blend width.

So a cached volume SHALL be used to seed an evaluation ONLY where it stores every sample that evaluation needs. Everywhere else the prefix SHALL be evaluated from its compiled form instead, which is exact. Both paths SHALL produce the walk's answer; only the cost SHALL differ.

The decision SHALL be made per WINDOW of points rather than per point, so that the fast path stays a straight loop and the slow path is one whole extra evaluation rather than a branch inside it. Where a window is a run of storage units, the decision SHALL be made per UNIT, so that one uncovered unit does not drag an entire run onto the slow path.

#### Scenario: A window the volume covers is seeded from it
- **WHEN** points are asked for that lie inside the cached prefix's stored region
- **THEN** the answers match the full walk to floating-point rounding, and the fast path was the one taken

#### Scenario: A window the volume does not cover falls back and is still exact
- **WHEN** points are asked for that lie outside the cached prefix's stored region
- **THEN** the answers match the full walk, and the composition counters record that the slow path was taken

#### Scenario: Seeding from an uncovered volume is what the rule prevents
- **WHEN** a suffix is deliberately seeded from the cached volume outside its stored region
- **THEN** the result differs from the full walk by many cells, which is the error the rule exists to exclude

### Requirement: A stand-in for a prefix is built on the consumer's own lattice and without post-processing
A cached prefix reproduces an ACCUMULATOR — the value the suffix folds onto — and not a finished field, so the two conveniences a finished bake applies SHALL be withheld.

The samples SHALL NOT be replaced by the distances they imply. That post-process is correct for a volume that is about to BE a layer and wrong for one that has to reproduce the number the prefix produced; applied here it moves the composed answer by cells rather than by rounding. Any pruning that depends on it SHALL be withheld with it, both because it is unsound without it and because dropping stored units would shrink the region the coverage rule above calls covered.

The stand-in SHALL be sampled over the WHOLE LAYER'S region rather than over the prefix's own. Two sampled volumes share a lattice exactly when they share a region and a resolution, and a seed read off a shared lattice IS the stored sample where one read off a lattice half a unit away is an interpolation of two. It is also the only region that is large enough: a suffix grows the surface into places the prefix never reached, and the prefix's own bounds are precisely the region that fails to cover that.

Whether the stand-in carries colour SHALL be decided from the nodes the PREFIX covers, not from the whole layer: a prefix that is all one colour must not grow a colour channel because an item in the suffix is red.

#### Scenario: The stand-in is exact on the lattice it was built for
- **WHEN** a volume is built through a cached source over the same region and resolution the prefix was cached at
- **THEN** it agrees with the same volume built from a full walk to floating-point rounding

#### Scenario: Post-processing the prefix is what the rule prevents
- **WHEN** a prefix is deliberately built with its samples replaced by the distances they imply
- **THEN** the composed result differs from the full walk by about two cells on its own lattice

#### Scenario: The prefix's own bounds are what the rule prevents
- **WHEN** a prefix is deliberately built over its own padded bounds rather than the layer's region
- **THEN** the composed result differs from the full walk everywhere, by a fraction of a cell, because the lattices no longer coincide

### Requirement: A cached prefix is identified by its content, its boundary and its resolution
An entry SHALL be usable only while it provably still describes the roots it was built from. It SHALL be keyed by the layer, the boundary and the sampling resolution — the same prefix sampled at two resolutions is two entries and neither is wrong — and it SHALL carry a digest of the layer's own properties plus the roots BEFORE the boundary and nothing after them.

The digest SHALL be re-checked whenever an entry is looked up, and a mismatch SHALL drop the entry rather than serve it. Invalidating by command MAY exist as an optimisation, but SHALL NOT be the guarantee: a missed invalidation is wrong geometry, a redundant one is only slow, and a digest computed from what the layer holds right now cannot be forgotten the way an invalidation call can.

The digest SHALL cover the layer's own properties as well as its roots, because a prefix built under one mirror or radial setting and reused under another is a different field. It SHALL read the CONTENT rather than the identity of the shared edit list, so that an edit made through a SIBLING INSTANCE — which shares that list, leaving the address unmoved — is seen.

The prefix digest SHALL be separate from any whole-layer digest, and appending a root after the boundary SHALL NOT move it. Appending is what a stroke IS, and a whole-layer key would throw away a still-valid entry on every dab. The number of roots covered SHALL be part of the digest as well as bounding it, so that two prefixes agreeing on their shared roots but ending at different boundaries are told apart. A prefix digest taken over ALL of a layer's roots SHALL equal that layer's whole-layer digest.

#### Scenario: Appending after the boundary keeps the entry
- **WHEN** a root is appended to a layer whose prefix is cached
- **THEN** the cached prefix is still served, and it is still the full walk's answer

#### Scenario: Editing before the boundary drops the entry
- **WHEN** a root inside the cached prefix is edited
- **THEN** the entry is dropped and reported as an invalidation, and the next answer is the full walk's

#### Scenario: An edit through a sibling instance is seen
- **GIVEN** two layers sharing one edit list, one of them with a cached prefix
- **WHEN** an item inside that prefix is edited through the other layer
- **THEN** the cached entry is not served

#### Scenario: A layer property moves the digest
- **WHEN** the layer's transform, mirror or radial settings change
- **THEN** the cached entry is not served

### Requirement: Where the boundary goes, and whether to have one, is a policy the host sets
The library SHALL NOT decide on its own to cache anything. A policy SHALL state the sampling resolution, how much history is worth caching at all, how many roots stay live in front of the boundary, and a ceiling in bytes.

A value-initialised policy SHALL cache NOTHING, which is the safe reading of a struct nobody filled in. A byte ceiling of zero SHALL mean the cache is OFF rather than unbounded, because a cache with no ceiling is a leak on a device with a memory budget. A resolution SHALL be required and non-zero: a document has no intrinsic sampling resolution, and inventing one here would fix a cache's fidelity at a number nobody chose.

The boundary SHALL be a count of TOP-LEVEL roots and SHALL NOT fall inside a group, because the accumulation the seed continues folds at top-level root boundaries and a group is one root. A boundary that would leave an empty prefix, or leave no suffix for a seed to fold onto, SHALL be declined rather than built.

The cache SHALL evict to its ceiling, least-recently-used first, and lowering the ceiling SHALL evict at once. A ceiling too small to hold a single entry SHALL decline rather than serve an entry it has already discarded.

#### Scenario: An empty policy caches nothing
- **WHEN** a source is opened with a value-initialised policy
- **THEN** nothing is cached, nothing is built, and every answer is the full walk's

#### Scenario: The ceiling evicts, and zero means off
- **WHEN** more prefixes are cached than the ceiling admits
- **THEN** the least recently used entries are evicted until the total fits, and a ceiling of zero holds nothing at all

#### Scenario: A layer with too little history is declined
- **WHEN** a layer holds fewer roots than the policy considers worth caching, or fewer than the live suffix it asks to keep
- **THEN** no boundary is chosen and no volume is built

### Requirement: Opening a source never pays for a bake
Opening the accelerated view of a layer SHALL use an entry that already exists and SHALL NOT create one. Building is a separate call a host makes when it has somewhere to put the work.

The reason is the caller this exists for: opening happens at the start of a gesture, where an artist is already waiting, and building there would reinstate the whole-layer cost the cache exists to remove. Until a host schedules a build, every answer is the full walk — correct, and slower.

A build SHALL be cancellable, and a cancelled build SHALL cache nothing rather than cache a partial result.

#### Scenario: Opening a source does not build one
- **WHEN** a source is opened against an empty cache with a policy that would admit a prefix
- **THEN** no volume is built, the cache's build count is unchanged, and the source reports that it is not accelerated

#### Scenario: A cancelled build caches nothing
- **WHEN** a build is run against a token that is already set
- **THEN** the cache holds no entry for that layer and the next answer is the full walk's

### Requirement: A cache reports whether it is working, not only whether it is correct
The cache SHALL expose counters for what it holds and for how it is being used: entries, bytes, hits, misses, builds, evictions, invalidations, and separately the number of evaluation windows that were SEEDED from a cached volume against the number that FELL BACK to evaluating the prefix.

The composition counters are not an optimisation detail. A cache whose fallback rate is high is producing correct answers at the uncached cost — it is not wrong, it is NOT WORKING — and that is a different fault with a different cure. Because the output is identical either way, nothing else in the system can distinguish the two, so the counters SHALL be part of what the cache offers rather than a debug aid behind a build flag.

#### Scenario: A hit and a miss are told apart
- **WHEN** a prefix is asked for before and after it is built
- **THEN** the first is counted as a miss and the second as a hit

#### Scenario: Falling back is counted apart from seeding
- **WHEN** points inside and outside the cached prefix's stored region are evaluated
- **THEN** the seeded and fallback window counts move independently, and both sets of answers match the full walk

### Requirement: An accelerated source states which promise it is keeping
A source backed by a sampled prefix is a SAMPLING source, and it SHALL be documented as answering three different ways rather than as answering "the field":

- ON the lattice the prefix was built for, it is the full walk's answer to floating-point rounding. This is what makes it usable by a consumer whose own working storage is that lattice, because a seed read there is the stored sample rather than an interpolation of two.
- BETWEEN lattice points, it is ordinary interpolation of stored samples — about a quarter of a cell at a fine resolution. This is the same fidelity a consolidation of the same prefix would have, and it is NOT the walk's answer. A consumer that needs the exact field at arbitrary points SHALL be able to get it, by using no cache.
- OUTSIDE the prefix's stored region, it is exact, because the coverage rule sends those windows to the compiled prefix.

The frame SHALL be the layer's OWN — the layer visible and its own transform identity, exactly as sampling a layer for consolidation does — so that a volume built through the source composes with, and can be installed under, that layer the same way a consolidated one can.

#### Scenario: The three regimes are what they say they are
- **WHEN** an accelerated source is asked for points on its lattice, between its lattice points, and outside the prefix's stored region
- **THEN** the first and third match the full walk to rounding, and the second differs by no more than interpolation between stored samples

#### Scenario: A volume built through the source can be installed under its layer
- **WHEN** a volume is built through an accelerated source and installed as its layer's content
- **THEN** nothing has moved: the layer's own transform still applies exactly once
