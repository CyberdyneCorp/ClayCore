# Proposal: brick-mesh attribute taps go to the backend as one batch

## Why

The brick-mesh attribute pass (gradient normals + vertex colors in
`apply_brick_attributes`) already compiles ONE culled tape per involved brick,
shared by normals and colors, and with the cull index those compiles are cheap
(~0.8 ms of a 98.8 ms dense re-mesh). What kept the pass slow was the
evaluation: five field taps per vertex (one color eval + four tetrahedron
gradient taps) ran one vertex at a time on one core, while the refill over
the same bricks evaluated the same culled tapes across the whole thread pool.
Measured with the v0.29 harness (M2 Max, medians, C ABI, 48-brick dab,
100 → 10000 accumulated stamps): the mesh phase of a dab grew 7.3 → 98.8 ms —
a 9.30 us/item slope, 6.7x the refill's 1.40 us/item — because the per-brick
culled tapes legitimately grow with local stamp density and every vertex paid
them serially.

## What changes

The backend interface gains `eval_points_batch`: many point runs, each
against its OWN tape — the attribute-pass shape, mirroring what
`eval_grid_batch` is for refill. The default implementation loops
`eval_points` per run, so every backend answers and answers identically; the
CPU backend overrides it to dispatch the FLATTENED batch across its thread
pool, because a per-run barrier leaves most cores idle on runs of a few
hundred vertices.

`apply_brick_attributes` still groups vertices by the brick owning their
position and compiles one culled tape per group (shared by normals and
colors, through the cull index and plan); it now hands every group's points
to the CPU backend as one `eval_points_batch` call instead of evaluating them
in a serial loop.

Results are unchanged, not traded: each point is evaluated by exactly one
chunk with the reference arithmetic — same tape, same point, same
`gradient_eps`, same tetrahedron taps — so the output is byte-identical to
the serial loop's, and the mesh (positions, normals, colors, indices) is
byte-identical to what the previous code produced. Verified by memcmp over a
fixed corpus against the pre-change library, and regression-tested in
`tests/unit/test_points_batch.cpp`.

Measured after (same harness, same machine): the dab's mesh phase is
3.1 → 17.5 ms over the same axis — slope 1.44 us/item, 1.29x the refill's —
and a dense 10000-stamp dab's mesh+refill fits 30 ms end-to-end instead of
115 ms.

## Impact

- Affected specs: `evaluation-backends` (interface gains the batched point
  form)
- Affected code: `include/clay/eval/backend.h` (`PointBatchQuery`,
  `eval_points_batch` default), `backends/cpu/cpu_backend.cpp` (flattened
  pool dispatch), `src/mesh/marching.cpp` (attribute pass batches through the
  backend), `benchmarks/bench_main.cpp` + `tools/check_bench.py` (dense
  mesh-vs-refill ratio gate), `docs/05-claycore-library.md`
- No C ABI change; mesh output byte-identical
