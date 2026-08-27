# build-packaging

## ADDED Requirements

### Requirement: a ThreadSanitizer preset, run in CI

The repository SHALL provide a `tsan` CMake preset — configure, build and test —
that builds the library and the whole test suite with ThreadSanitizer, and CI
SHALL run it on every pull request.

The reason it is a preset and a job rather than a thing to remember: this
library's threading guarantees are guarantees about LOCKING, and a lock that
stops being taken still returns the right values. The regression case for
`clay_brick_cache_eval_requests` refilling off the cache lock passes its value
assertions with the fix reverted — doctest reports SUCCESS — while a raw pointer
into a hash map another thread may rewrite is live across the unlock. Nothing
but a race detector distinguishes those two states, so a race detector has to
run automatically or the guarantee has no standing guard.

The preset SHALL be an optimised build with debug info rather than a debug
build: races are timing-dependent, so the build worth racing is the one that
ships, and ThreadSanitizer's own slowdown over a debug build would put a whole
suite past what a pull request should cost.

The suite SHALL be clean under it — no reported data race — not merely the one
case that motivated the job.

Documentation SHALL record that the run needs ASLR disabled (`setarch -R`, or
`vm.mmap_rnd_bits` lowered). ThreadSanitizer maps fixed shadow regions and
aborts with `FATAL: ThreadSanitizer: unexpected memory mapping` before any test
executes on a kernel that hands out mappings with more entropy than it expects.

#### Scenario: the preset exists and builds with the sanitizer on

- **WHEN** `cmake --preset tsan` runs
- **THEN** it configures with ThreadSanitizer enabled
- **AND** `cmake --build --preset tsan` builds the library and the test suite

#### Scenario: CI runs it per pull request

- **WHEN** a pull request is opened
- **THEN** a CI job configures, builds and runs the whole suite under the `tsan`
  preset
- **AND** the job fails if any data race is reported

#### Scenario: a fix reverted is a job failed

- **GIVEN** the resumed refill's seed copy reverted, so a pointer into the seed
  store stays live across the cache-lock release
- **WHEN** the `tsan` job runs
- **THEN** it reports the race and fails, while a run of the same case without a
  sanitizer passes

### Requirement: the resumed-refill benchmark's fixture guard is gated

`tools/check_bench.py` SHALL gate the share of bricks the still-window resumed
refill benchmark walks IN FULL, as it already gates the moving-window pair.

The benchmark primes every brick before its timed region so that the region
measures the resumed path and nothing else. A fixture that stopped resuming
would report the FULL path's time under the resumed path's name, and no timing
threshold can tell that from a slower runner — a ratio of counts can, and says
the same thing on any machine.

#### Scenario: the fixture stops resuming

- **WHEN** the still-window benchmark's bricks are walked in full rather than
  resumed
- **THEN** the benchmark gate fails, naming the counter, its value and its
  ceiling
