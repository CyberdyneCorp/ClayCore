# Proposal: a resumed refill should not hold the cache lock while it evaluates

## Why

`clay_brick_cache_eval_requests` ran its whole per-brick resumed loop inside one
`std::lock_guard<std::mutex> lock(doc->cache_lock())` — `compile_layer_suffix`
AND `eval_points_seeded` for every brick, on one thread.

Two things are wrong with that, and only one of them is about speed.

**It is serial where the path it replaces is not.** A brick that cannot resume
goes to `eval_requests_in_chunks`, which hands the batch to
`Backend::eval_grid_batch` — on the CPU a row-level `parallel_for` across the
whole batch, bricks and rows within them spread over the pool. So the fast path
used one core where the slow path used every one.

**It blocks unrelated readers.** `cache_mutex_` also guards the tape cache, the
cull index and the append log, and `clay_eval_points` on another thread takes
it. The ABI promises (clay.h, THREADING) that any number of threads may run
refills and reads against one const document; holding the lock across an
evaluation makes that promise cost whatever the evaluation costs.

The lock was held for a reason: `seed_for` hands out a RAW POINTER into the
`resume_` map and `store_active` writes back through it, so releasing it
mid-loop would leave a live pointer into a container a concurrent refill may be
writing.

## What

Three phases, replacing one.

1. **Under the lock**: resolve the plans (one per distinct stored revision, as
   before) and COPY each brick's seed out. The copy lands in the buffer the
   evaluation will write its answer to — the caller's own output slot when there
   is no layer beneath, staging when there is one and a union still has to be
   applied. `eval_points_seeded` reads a block's seed into its stack before it
   writes that block's result, so a seeded walk may run in place: that is #306's
   open question 7, and it is what makes the copy cost nothing in the common
   single-layer case beyond one `memcpy` into a buffer that had to be written.
2. **Released**: compile and evaluate. Only the document and the cull index
   snapshot are read, both of which the full path already reads unlocked.
3. **Retaken**: `store_active` for every brick that produced an answer, with the
   revision re-checked — the check `store_seeds` already makes for the full
   path.

The deferred phase goes over `clay::parallel::ThreadPool` only when it is worth
a dispatch. Below that it runs on the calling thread — still off the lock.

The whole of it moves out of `clay_brick_cache_eval_requests` into a
`resume_bricks` helper, which is what keeps the entry point readable: it was 70
by cyclomatic count before this change and is 47 after, with the helper at 37.

## Measured

24-thread desktop, dim-8 lattice, 5,000-item sculpt, `BM_BrickRefillWindow`.
These are PHASES inside one call, A/B interleaved three times with the box at
load average 0.6–2.6, `uptime` read before and after every run.

| window / suffix | lock held, before | lock held, after | deferred, after |
|---|---:|---:|---:|
| 1 brick, 1-dab | 51.6 µs | 50.1 µs | 0.6 µs, serial |
| 48 bricks, 1-dab | 68.4 µs | 52.1 µs | 18.7 µs, serial |
| 12 bricks, 16-dab | 282.3 µs | 242.0 µs | 19.4 µs, serial |
| 48 bricks, 16-dab | 235.2 µs | 210.4 µs | 31.3 µs, pooled |

Most of what remains under the lock is the per-revision cull index, which the
call copies to extend and which stays there because the cache is what the lock
is for. Net of it, the lock holds:

| window / suffix | before | after | |
|---|---:|---:|---:|
| 1 brick, 1-dab | 0.8 µs | 0.5 µs | |
| 48 bricks, 1-dab | 22.4 µs | 6.8 µs | 3.3x |
| 12 bricks, 16-dab | 19.4 µs | 2.4 µs | 8x |
| 48 bricks, 16-dab | 65.8 µs | 10.0 µs | 6.6x |

And the deferred work itself, where the pool is taken: 48 bricks at a 16-dab
suffix, **65.8 µs serial to 31.3 µs pooled**, 2.1x.

## Where the crossover is

An empty `parallel_for` over 48 units costs **16–19 µs** on this machine, and
that is the whole of the question:

| | units | serial | pooled |
|---|---:|---:|---:|
| 48 bricks, 16-dab suffix | 393,216 | 66 µs | **30 µs** |
| 12 bricks, 16-dab suffix | 98,304 | **21 µs** | 25 µs |
| 48 bricks, 1-dab suffix | 24,576 | **19 µs** | — |

The middle row is why this is gated rather than always taken: the pool costs
more than it saves there, and burns two dozen cores to do it. A UNIT is one
sample times one appended item — the shape of the work the walk does — so the
gate does not have to be restated for a different lattice size or a longer
suffix. It sits at **262,144**, about three times the dispatch. Below it the
loop is serial and off the lock; the copy that buys that costs about **0.05 µs a
brick**, which is inside the noise at one brick.

## Where the crossover is, with several threads refilling at once

The table above calibrates the gate against ONE caller, and this change exists
so that there can be several. That needs its own measurement, because
`clay::parallel::ThreadPool` holds one `current_` job slot: a dispatch from a
second host thread REPLACES the advertised job, so workers that have not woken
for the first never will (`thread_pool.h` says so in its own header comment).
`in_job()` guards nesting on ONE thread and does not guard this. The result is a
degradation toward serial, not a deadlock — so the question a gate has to answer
is whether it degrades PAST the serial branch it is choosing between.

It does not, at any concurrency this machine can produce. T threads each
refilling the same 48-brick window at a sixteen-dab suffix over a 5,000-item
document; three libraries — `main`, this branch, and this branch with the gate
forced to never pool — interleaved run for run; minimum over 25 repetitions of
60 rounds; box at load average 3–9, `uptime` read before and after:

| threads | main | pooled | serial | pooled / serial | pooled / main |
|---:|---:|---:|---:|---:|---:|
| 1 | 93.1 µs | 74.9 µs | 94.8 µs | **0.79** | 0.81 |
| 2 | 110.1 µs | 84.7 µs | 110.6 µs | **0.77** | 0.77 |
| 3 | 115.1 µs | 102.7 µs | 121.8 µs | **0.84** | 0.89 |
| 4 | 127.1 µs | 115.2 µs | 130.8 µs | **0.88** | 0.91 |
| 6 | 142.6 µs | 142.7 µs | 155.2 µs | **0.92** | 1.00 |
| 8 | 164.1 µs | 158.7 µs | 186.9 µs | **0.85** | 0.97 |
| 12 | 199.9 µs | 197.7 µs | 233.9 µs | **0.85** | 0.99 |

The pooled branch beats the serial one at every thread count, so **the constant
holds** and 262,144 stays where the single-caller measurement put it.

`main` is the column that stops improving: it holds the mutex across its
evaluation, so T concurrent refills are T serial refills. Twelve threads cost it
2.1x one thread and cost this branch 2.6x — the same work actually overlapping.
Against `main` the margin narrows from 0.81 to a wash by six threads, which is
this change's honest ceiling in that shape and is worth stating plainly rather
than quoting the single-threaded 0.81 as the number.

The OTHER shape is the one clay.h recommends — fan out over REQUESTS, one batch
split across threads — and there the gate settles it without any of the above:
48 bricks over two threads is 196,608 units a slice, under the gate, so no
thread dispatches and the collision cannot arise. Measured at 0.55–0.77x `main`
from one thread to six.

Read the ratios, not the times. Two builds running IDENTICAL code — the split
shape at two threads and up, where both are below the gate — measure 1.01–1.07x
apart on this box. That is the harness's noise floor, and it is wider than
several of the differences the tables above would otherwise invite reading.

## What this is NOT

**It is not the largest cost under that lock.** Obtaining the per-revision cull
index copies it — 45 µs at 5,000 items for a one-dab suffix, 170–260 µs for a
sixteen-dab one, which is 65–96% of the lock hold in every measurement above.
That is a different issue and this change does not touch it. What #348 claimed —
that a host submitting a large dirty window pays the per-brick loop serially per
brick — is true and is fixed here, but at a one-dab suffix that loop was 0.39 µs
a brick to begin with, and the frame is not waiting on it.

**It is not a change to what a refill answers.** Results stay bit-identical to
the serial path; that is the contract, and the existing parity tests plus a new
concurrent one hold it.

## Coverage

Nothing drove the refill path concurrently: `test_c_tape_cache.cpp` had
concurrent-reader cases for the tape cache and none for a refill racing a
reader, so the old locking was untested rather than proven.

A new case runs three refill threads and three `clay_eval_points` threads
against one document over thirty rounds of a stroke, alternating a 64-brick
window (which takes the pool) with a 4-brick one (which does not), and checks
every answer bit-for-bit against a fresh document that never resumed. It is
clean under `asan-ubsan` and under a ThreadSanitizer build.

That it has teeth is shown by mutation: with the copy reverted so the raw
pointer into `resume_` stays live across the unlocked evaluation, TSan reports
the race between `store_active`'s write and `eval_points_seeded`'s read. The
value assertions still pass in that mutant — every thread computes the same
seed — which is precisely why a sanitizer, and not a value check, is what guards
this.

Which means the guard only exists if the sanitizer RUNS. There was no `tsan`
preset and no ThreadSanitizer job, so the spec's "no data race is reported"
scenario was a scenario nothing checked. This change adds both: a `tsan`
configure/build/test preset (RelWithDebInfo, `CLAY_SANITIZE=thread`) and a CI
job that runs the WHOLE suite under it — everything else threaded in this tree
has been in the same position the refill was, exercised concurrently and never
checked. Measured at 6 minutes on a 24-thread desktop: 1501/1501, zero TSan
warnings.

The job runs under `setarch -R`, which is not optional. ThreadSanitizer maps
fixed shadow regions and a kernel handing out mappings with more entropy than it
expects — Ubuntu 24.04's `vm.mmap_rnd_bits=32` — aborts the process with
`FATAL: ThreadSanitizer: unexpected memory mapping` before one test runs. It
exits 66, so ctest does fail rather than passing silently, but the log says
nothing about a race and the job would be gating nothing.

`BM_BrickRefillWindow` reports `refilled_frac` beside `resumed_frac`, and
`tools/check_bench.py` gates it at 0.05 — the same mechanism and the same reason
as the moving-window pair. The benchmark primes every brick so its timed region
is the resumed path alone; a fixture that quietly stopped resuming would report
the full path's time under the resumed path's name, and only a ratio of counts
can tell that from a slower runner.

The in-place property gets its own case in `test_suffix_tape.cpp`, across block
boundaries as well as whole batches, which fails on eight assertions when
`walk_blocked` is mutated to write a block's destination before reading its
seed. It is also stated where it is DECLARED, in `include/clay/eval/backend.h`:
the property is now load-bearing at a distance, and a future optimisation that
hoisted the output store above the seed load, or added `restrict`, would break
the refill silently with only that one test to catch it.
