# c-abi

## ADDED Requirements

### Requirement: a resumed refill does not hold the document cache lock while it evaluates

`clay_brick_cache_eval_requests` SHALL NOT hold the document's cache mutex while
it compiles a brick's suffix or evaluates it. The mutex SHALL be held only to
resolve the resume plans and read the seeds, and again to store what the bricks
reached.

A seed SHALL be COPIED out of the seed store while the mutex is held. No pointer
into the seed store may be read after it is released: the store is a hash map
another refill on another thread may be writing to, and the entry a raw pointer
names can be rewritten or evicted under it.

Before a brick's answer is kept as its next seed, the document's revision SHALL
be re-checked against the one the plan was made at — the same check the full
path's store makes — and the answer discarded as a seed, but still returned to
the caller, when it has moved.

What a refill returns SHALL NOT depend on any of this: a resumed brick is
bit-identical to what the full walk gives, whether or not other threads were
refilling or reading the same document at the time.

#### Scenario: a refill racing readers

- **GIVEN** a document with a stored seed for every brick of a window
- **AND** one item is appended to the active layer
- **WHEN** several threads refill that window while several others evaluate
  points against the same document
- **THEN** every refill is bit-identical to the same window refilled from a
  document holding the same items that never resumed anything
- **AND** every point evaluation agrees with the same document read alone

#### Scenario: no data race under a sanitizer

- **GIVEN** the concurrent refill above
- **WHEN** it is run under ThreadSanitizer
- **THEN** no data race is reported

#### Scenario: a seed the store no longer holds

- **GIVEN** two threads refilling one document at once
- **WHEN** one stores a brick's new value while the other is evaluating from
  that brick's seed
- **THEN** the evaluating thread reads its own copy of the seed and is
  unaffected

### Requirement: the deferred phase is parallel only when it is worth a dispatch

The compile-and-evaluate phase SHALL run over the shared thread pool
(`clay::parallel::ThreadPool`) only when the work it would spread exceeds the
pool's dispatch cost, measured as a count of samples times suffix length summed
over the bricks. Below that threshold it SHALL run on the calling thread — off
the lock either way.

Whether the pool is used SHALL NOT change what a refill answers.

#### Scenario: a small window

- **GIVEN** a refill of a few bricks whose suffix is one appended item
- **WHEN** the deferred phase runs
- **THEN** it runs on the calling thread, without a pool dispatch
- **AND** the values are the ones the pooled path would give

#### Scenario: a large window

- **GIVEN** a refill of a window large enough, or a suffix long enough, that the
  deferred work exceeds the threshold
- **WHEN** the deferred phase runs
- **THEN** it is spread over the thread pool
- **AND** the values are the ones the serial path would give
