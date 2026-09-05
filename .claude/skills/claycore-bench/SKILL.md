---
name: claycore-bench
description: Measure a claycore performance change honestly — the A/B worktree harness against the C ABI, the in-tree clay_bench gate, sample counts that do not lie, and the fixtures that quietly measure nothing. Use when asked to prove a speedup, investigate a "got slower" report, or size a benchmark case.
---

# Measuring a claycore perf change

Two tools, for two questions.

## In-tree: `clay_bench` and the regression gate

Answers "does this change move a case the repo already tracks".

```sh
cmake --build build/bench --target clay_bench -j8
./build/bench/clay_bench --benchmark_filter=BM_Whatever --benchmark_min_time=0.2s \
  --benchmark_out=/tmp/b.json --benchmark_out_format=json
python3 tools/check_bench.py /tmp/b.json
```

- **Benchmark out of `build/bench`, never `build/cpu-only`.** The latter has
  `CLAY_BUILD_BENCHMARKS=OFF` in its cache, so `cmake --build --target
  clay_bench` prints "Nothing to be done", the months-old binary runs happily,
  and an A/B against it measures nothing while looking like a clean pass. The
  tell is `check_bench.py` reporting "benchmark missing from results" for cases
  that plainly exist in `benchmarks/bench_main.cpp`.
- The full suite at `--benchmark_min_time=0.2s` takes **~2 hours** on the
  M-series box (the `BrickRefillWindow` family dominates). Iterate with
  `--benchmark_filter`; run the full gate once, in the background, started
  early.
- `check_bench.py` fails `BM_MetalTapeResident` in any build without the Metal
  backend. That failure is on `main` too — diff against `main` before calling it
  yours.

**Gate on counts, not on the clock, wherever a count exists.** Refills over a
drag, bricks proven uniform, nodes visited, layer walks, tape params: these are
deterministic on every machine and a wall-clock bound has to be so loose it
catches nothing. Where a change's whole claim is a *count*, the test asserts the
count and the benchmark carries the time beside it only to catch the categorical
failure. `clay_document_extent_stats`, `clay_brick_stats` and
`mesh::BvhWalkStats` exist for exactly this.

## Cross-revision: the A/B worktree harness

Answers "is this branch faster than that one", at a scene bigger than the
in-tree benchmarks, on the surface the iPad app actually uses.

```sh
git worktree add /tmp/wt-before <commit>
git worktree add /tmp/wt-after  <commit>
# build each: cmake --preset metal && cmake --build --preset metal
clang++ -std=c++20 -O2 -o bench-before bench.cpp \
  -I/tmp/wt-before/bindings/c -I/tmp/wt-before/include \
  /tmp/wt-before/build/metal/libclaycore.a \
  /tmp/wt-before/build/metal/_deps/meshoptimizer-build/libmeshoptimizer.a \
  -framework Metal -framework Foundation
```

The C ABI is compiled *into* `libclaycore.a`, so a harness links with no extra
target — and driving the public ABI keeps **one harness source valid on both
revisions**, which internal C++ does not.

**How to not lie to yourself:**

- **200 samples per point, and discard the whole first process run.** At 60
  samples with no warm-up the run-to-run p50 spread is ~35%, which reads exactly
  like "the change had no effect". That cost two wrong conclusions in one day:
  removing `touch_region` and removing both `command_influence_bound` calls each
  looked like noise at 60 samples and showed a clean 1.34x step at 200.
- **Interleave the binaries in one loop** (`for i in 1 2 3; do before; after;
  done`) so machine drift hits both arms equally. Read the settled repeats.
- **Report medians, not means** — a sample occasionally lands on an efficiency
  core.
- **Warm the Metal shader cache** before timing anything: first run on a fresh
  machine has cost 48 extra seconds (53 s vs 5.3 s warm).
- `clay_list_backends` confirms what actually registered, so a Metal build that
  silently fell back to CPU cannot be mistaken for a Metal result.

## The fixture is usually the bug

More perf investigations here have died on the fixture than on the analysis.

- **A fixture must reach the path.** `item_geometry_bound` does real work only
  for strokes, sweeps, mirrored items and deformer chains — an A/B built from
  spheres and boxes showed *no regression* for a defect that was real and 20x.
  Use `CLAY_PRIM_STROKE` with a curve when the cost you are hunting is a
  geometry bound.
- **A cold cache is not a warm one and the case must say which it means.** A
  latency case that resets the document every iteration can never reach the
  append path: the reset *is* the invalidation.
- **A drag cannot see its own invalidation.** The wide seed drop from frame N is
  paid by frame N+1, so gate the pair at equal brick count.
- **An append into a group is not an append** — `tail_append` needs a root-list
  parent, and a dab inside a group costs ~90x.
- **The bound must exist before you measure the refill.** Marking a layer dirty
  does nothing when its influence bound is infinite, so both arms have to be
  cold-filled with an explicit region or one of them tracks zero bricks.

## Reporting

State the machine, the backend, the sample count and the statistic, every time.
"1.7x" with none of those is not a measurement. Mac numbers are never device
numbers — see the `claycore-device-gate` skill for anything about latency on the
tablet.

A budget derived from one run is a budget that cannot fail. Derive a replacement
from the top of the observed band, keep at least a 0.5k margin, and re-check that
the case still sits comfortably above the 0.125 ms gating floor after the win.
