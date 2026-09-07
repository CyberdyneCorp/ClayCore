## Why

**The stage timings exist, are good, and no host can read them.**

`mesh::SculptStage` names thirteen stages and `mesh::StageTelemetry` collects
nanoseconds and call counts per stage, with no clock read at all when nothing is
measuring. That is the hard half and it is already done. What is missing is
everything that would let it be used:

| | today |
|---|---|
| consumers of `StageTelemetry` in the whole tree | **one** — `benchmarks/bench_extreme_poly.cpp` |
| reaches the C ABI | **no** |
| reaches pyclay | **no** |
| `DynamicSculptor` (the adaptive surface) instrumented | **no** — mesh and multires only |
| tests asserting on any stage | **none** |
| counters saying WHY a stage changed | scattered across six accessors with six shapes |

So a host doing performance work has total dab time and nothing else, which is
the one thing this milestone says must stop. And this library has shipped that
failure before: the engine can do it and the host cannot reach it.

**Nothing catches a stage that stopped being timed.** Thirteen stages, one
consumer, zero tests. A stage whose `StageTimer` is deleted in a refactor
reports zero nanoseconds and zero calls, which is indistinguishable from a stage
that did no work — and the only reader is a benchmark nobody runs per commit.

**The adaptive surface is uninstrumented.** `DynamicSculptor` carries no
telemetry pointer, so the representation whose per-dab cost is hardest to
predict — it splits, collapses and flips as it goes — is the one with no stage
breakdown at all.

## What Changes

**Counters beside the clocks, because a duration alone cannot say why.**
`mesh::SculptCounters`, filled by the same three sculptors through the same
borrowed-pointer discipline: null costs one predictable branch and no work.

```text
vertices considered      vertices affected
faces touched            chunks touched
neighbours gathered      kernel passes
splits / collapses / flips        detail blocks touched
history bytes            scratch high-water
```

Several of these already exist and are being COLLECTED rather than invented —
`anchor_measurements`, `workset().size()`, `write_region().size()`,
`dirty_chunks().size()`, `BvhWalkStats`, `PeakTelemetry`. Six shapes become one
record a regression can read.

**`DynamicSculptor` gets the same vocabulary**, so a row from each of the three
representations can be compared.

**It crosses the ABI.** `clay_mesh_sculptor_stage_report` and its adaptive and
multires counterparts, a `struct_size` descriptor carrying both the times and
the counts, and the pyclay pair.

**Tests assert the counts, not the clocks.** A stage that stopped being timed
becomes a failing test rather than a zero nobody reads.

**Machine-readable output** from the benchmark driver, so a CI comparison is a
diff of numbers rather than of prose.

## Capabilities

### Modified Capabilities
- `sculpt-runtime`: the per-stage breakdown covers all three representations,
  carries counts as well as times, and is gated by tests rather than by one
  benchmark.
- `c-abi`: a host can read the per-stage breakdown of a stamp it just made.

## Impact

- `include/clay/mesh/sculpt_common.h` — `SculptCounters` beside `StageTelemetry`.
- `include/clay/mesh/dynamic_sculpt.h`, `src/mesh/dynamic_sculpt.cpp` — the
  adaptive surface's stages and counts.
- `include/clay/mesh/sculpt.h`, `src/mesh/sculpt.cpp`,
  `src/mesh/multires_sculpt.cpp` — the counts.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`,
  `bindings/python/pyclay_module.cpp` — reachability.
- `benchmarks/bench_extreme_poly.cpp`, `tools/bench_extreme_poly.py` — the
  machine-readable rows.
- `tests/unit/` — the gates.
