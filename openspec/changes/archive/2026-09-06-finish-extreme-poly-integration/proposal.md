## Why

`add-extreme-poly-runtime` (v0.78.0, #423) shipped with three tasks open, and all
three were deferred for the same reason: they live in `src/mesh/sculpt.cpp`,
which two sibling branches were editing while it was written. Those branches
merged (#419, #417), the extreme-poly branch merged onto them, and the reason the
tasks were deferred no longer exists. This change closes them.

**3.1 — the hierarchy still scans.** The requirement is "brush volume → top-level
tree → candidate chunks → candidate vertices → exact footprint. Never a scan over
every vertex". Measured, it holds on the fixed mesh and the adaptive surface —
20M vertices against 1M at the same 20k footprint is 1.05x the dab — and is
**false on the hierarchy**: 0.49 ms at 100k level vertices against 1.58 ms at 1M
for the same 1k footprint, about 1.2 ns a vertex, which is a scan.

The cause is precise. `MeshSculptor::surface_index()` never builds a ray tree on
its own behalf, deliberately — a build is 689 ms against 1.24 ms saved per stamp,
so it would need ~550 stamps to break even. A host that PICKS gets one for free
because `raycast` builds it. `MultiresSculptor` is a caller that never picks, so
nothing ever builds one for a level, and every unseeded stamp falls into the
O(classes) scan inside `geodesic_region`.

3.2 shipped the seed that avoids the scan, and 3.1 stayed open for one reason:
**nothing supplies that seed automatically**. A stroke is dozens of dabs a
fingerwidth apart and every one of them re-finds an anchor the previous dab
already knew.

**7.2 — per-stage timing is six stages of fourteen.** Seed, chunk query, index
update, remesh, detail write and readback are timed because each is its own call.
The eight inside `MeshSculptor::stamp` — gather, geodesic, snapshot, weight,
alpha, automask, kernel, normals — are one bucket named `stamp*`. A total without
stages is not actionable, and the stage split is what located 3.1 in the first
place.

**The fixed sculptor does not mark its own chunks.** Task 2.5 integrated
`ChunkTable` with the adaptive surface and with each multires level and stopped
there. On the fixed mesh the table exists and nothing in `MeshSculptor` touches
it: `bench_extreme_poly.cpp:230` and `test_extreme_poly_scaling.cpp` mark chunks
from OUTSIDE after a stamp, which is host-side logic every host would have to
copy, and copy identically, to get a correct dirty stream.

## What Changes

- **A carried anchor.** `MeshSculptor` remembers the class its last stamp
  anchored on and starts the next stamp's search there instead of scanning.
  Reset when the class space is retired, which a hierarchy rebind already does.
- **It resolves to the SAME class the scan would have found**, by descending the
  adjacency from the carried anchor to the class nearest the new centre, rather
  than being used as the anchor directly. This is the whole design risk and §2 of
  `design.md` is about it: `geodesic_region` accumulates path distance FROM THE
  SEED, so a different anchor is a different falloff and a different dab. The
  three golden tables run on the no-index path, so "carry the previous anchor"
  as written in the source guide would have changed every sculpt result on every
  platform.
- **The eight remaining stages are instrumented** behind a null telemetry
  pointer, under one stage enum shared by the fixed, adaptive and hierarchy
  paths, so the benchmark's `stamp*` bucket resolves.
- **`MeshSculptor` marks its own chunks** — geometry over the write region,
  normals when they are refreshed, attributes when the verb painted — so a
  correct dirty stream is a property of the sculptor rather than of each host.
- **The `multires + layers` benchmark rows are run** and recorded. They were
  scripted and unrun.

**Not in this change: the reference iPad (7.8).** There is no device on this box.
The task stays open and says so; nothing here claims a device number.

## Capabilities

### Modified Capabilities
- `brush-engine`: the seed requirement gains "a stroke does not re-find an anchor
  it already has", with the bit-identity constraint stated as a requirement
  rather than left to the implementation.

## Impact

- `include/clay/mesh/sculpt.h`, `src/mesh/sculpt.cpp` — the carried anchor, the
  descent, the stage timers, chunk marking.
- `include/clay/mesh/multires_sculpt.h`, `src/mesh/multires_sculpt.cpp` — the
  anchor survives a rebind that does not retire the class space.
- `include/clay/mesh/sculpt_common.h` — the shared stage enum.
- `tests/unit/` — the bit-identity gate, the no-scan gate, the chunk-marking gate.
- `benchmarks/bench_extreme_poly.cpp` — the layered rows; the `stamp*` bucket splits.
- `docs/09-brush-latency-and-coverage.md` — the numbers.
- No ABI change and no format change. The three golden tables MUST NOT move.
