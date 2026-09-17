## Why

`openspec/ROADMAP.md` records the gap twice, and the live `brush-engine` spec
records it as a scenario: a stroke reaches the fixed mesh (`brush::apply_to_mesh`)
and the hierarchy (`brush::apply_to_multires`), and **there is no
`brush::apply_to_dynamic`**. So spacing, the pressure curve, taper, jitter,
steady stroke, accumulation, the stylus azimuth and the brush presets reach two
of the three mesh representations and not the adaptive one.

Counted on `origin/main` at `aafeccb6` (ABI 0.117.0):

| surface | fixed mesh | hierarchy | adaptive surface |
|---|---|---|---|
| C++ stroke consumer | `apply_to_mesh` | `apply_to_multires` | **none** |
| C ABI stroke | `clay_mesh_sculptor_apply_stroke`, `_apply_preset` | `clay_multires_sculptor_apply_stroke` | **none** (`clay_dynamic_sculptor_stamp` only) |
| pyclay stroke | `MeshSculptor.apply_stroke`, `.apply_preset` | `MultiresSculptor.apply_stroke` | **none** (`DynamicSculptor.stamp` only) |

A host can get SOMETHING today — `clay_stroke_resolve_full`, then one
`clay_dynamic_sculptor_stamp` per stamp — so the question this proposal had to
answer first is whether that is already good enough. It is not, and the reasons
are measured rather than argued.

### What a host's own loop gets wrong

Probe: a scratch binary linked against this tree's `libclaycore.a`, on a
`cube_sphere(24, 1.0)` (six 24x24 quad grids projected onto the unit sphere), default
`DynamicTopologySettings` (enabled, brush-relative, detail 8), preset radius
0.3. "HOST" is the loop a host writes from the headers: centre = stamp position,
radius and strength from the stamp, direction = motion since the last stamp.
"PROTO" is that loop with `apply_to_mesh`'s drag rules transplanted — Grab
centred on the first stamp, Snakehook centred on the VERTEX it is dragging. The
fixed-mesh rows are the same stroke on the same sphere through `MeshSculptor`.
Reach is the surface's furthest extent along the drag, as a share of the drag.

| stroke | path | reach | stamps that moved a vertex |
|---|---|---|---|
| Snakehook, pull out 0.8, 14 stamps | dynamic HOST | **42%** | 10/14, last stamp moved **0** |
| | dynamic PROTO | **96%** | 13/14 |
| | fixed, HOST loop | 50% | 11/14 |
| | fixed, `apply_to_mesh` | 98% | 13/14 |
| Grab, pull out 0.6, 11 stamps | dynamic HOST | 56% | 10/11 |
| | dynamic PROTO | 41% | 10/11 |
| | fixed, HOST loop | 66% | 10/11 |
| | fixed, `apply_to_mesh` | 41% | 10/11 |

Two readings:

1. **Snakehook is lost by a host loop on both representations**, and in exactly
   the way `stroke.cpp` already documents for the fixed mesh: the centre vertex
   moves by its falloff weight rather than the whole delta, the brush falls
   behind the cursor, and a few stamps later it is outside its own radius and
   moves nothing. The anchoring rule is the stroke's, it lives in `brush`, and a
   host cannot be expected to rediscover it.
2. **The transplanted rules make the adaptive surface agree with the fixed one**
   — Snakehook 1.770 vs 1.780, Grab 1.248 vs 1.247 — which is the
   cross-representation promise the brush engine makes. (Grab anchored on the
   first stamp reaching LESS than a following centre is a property of
   `apply_to_mesh` on `main`, reproduced above on the fixed mesh with no change
   applied. It is recorded under "What this does not do", not changed here.)

### The anchor dies under the remesher, and the fixed path never had to care

`apply_to_mesh` anchors Snakehook on a weld CLASS, and a fixed mesh never loses
one. The adaptive surface remeshes BEFORE and AFTER every Snakehook stamp, and a
collapse retires vertex ids. Counted with the PROTO loop, radius 0.25:

| path | detail | spacing | stamps | anchor died | reach, re-found at last anchor position | reach, re-found at previous stamp position | reach, never re-found |
|---|---|---|---|---|---|---|---|
| out 1.5 | 4 | 0.05 | 61 | 14 | 79% | 81% | **15%** |
| out 1.5 | 4 | 0.25 | 13 | 6 | 57% | 81% | 15% |
| out 1.5 | 8 | 0.05 | 61 | 8 | 93% | 94% | 18% |
| out 1.5 | 8 | 0.25 | 13 | 2 | 86% | 97% | 64% |
| diagonal | 4 | 0.25 | 9 | 3 | 59% | 83% | 41% |
| diagonal | 8 | 0.25 | 9 | 2 | 72% | 89% | 55% |
| sideways | 8 | 0.05 | 35 | 1 | 95% | 98% | 88% |
| out 1.5 | 16 | 0.05 | 61 | 1 | 99% | — | — |
| out 1.5 | 32 | 0.05 | 61 | 0 | 100% | — | — |

(Full tables: 32 rows at detail 4/8/16/32 and spacing 0.05/0.25 over four
paths. "anchor died" is the first column's policy; the other policies see
slightly different counts because they sculpt a different surface.)

So an adaptive Snakehook stroke **must revalidate its anchor every stamp**, and
a stroke that keeps a dead anchor's last position stops pulling: 15–18% reach
in the three rows where the anchor died 6–14 times, 41–88% in the rows where it
died 1–3 times.
Re-finding the nearest vertex to the previous STAMP position was never worse
than re-finding it at the dead anchor's last position in any of the sixteen
detail-4/8 rows (equal in eight, better in eight), by up to 24 points (57% -> 81%). That is the
rule this change adopts.

### Azimuth reaches the alpha only when the stroke carries it

Draw, an 8x8 half-on alpha, `rotate_to_azimuth`, tilt 0.5, two strokes
identical except azimuth 0 vs pi/2:

| `orient_alpha_by_stamp` | surfaces |
|---|---|
| off | bit-identical — the azimuth is dropped |
| on | differ — the azimuth reaches the alpha |

The same switch `apply_to_mesh` and `apply_to_multires` take, and the same
reason (`stroke.h`): inferring it from the quaternion is discontinuous at zero.

### What the ABI call does NOT buy: latency

Draw, 60 samples resolving to 14 stamps, radius 0.15, `cube_sphere(48)`, median
of 15: the C++ loop 72.975 ms, `clay_stroke_resolve_full` plus one
`clay_dynamic_sculptor_stamp` per stamp 73.065 ms — **1.001x**. The per-call
descriptor decode is noise against a remeshing stamp. The entry point is
justified by the stroke's MEANING (the drag anchor, the azimuth, the strength
composition) and by the parity gate, not by speed, and the proposal does not
claim otherwise.

That number was taken BEFORE the entry point existed, so it prices the host's
loop, not the call. Re-measured once the call was built (review): Draw at
strength 0.4, 60 samples resolving to 14 stamps, radius 0.15, detail 6,
`cube_sphere(48)`, Release `libclay_shared`, a fresh surface per run, the two
paths alternated per iteration, median of 30 after a warm-up (absolute times are
not comparable with the row above, whose brush settings differed): `clay_dynamic_sculptor_apply_stroke` 46.221 ms,
the resolve-then-stamp host loop 46.048 ms — **1.004x**, identical split counts
(1107). The call is the host loop's cost, as designed.

### Preconditions measured, so the tests can be exact

- **Determinism.** The same stroke run twice over identical surfaces with
  topology ON was bit-identical for Draw, Clay, Smooth, Grab, Snakehook and
  Flatten (taper, jitter and a pressure ramp on). So "a stroke equals its
  stamps applied one by one" can be asserted bit-exact rather than within a
  tolerance.
- **Undo.** A whole 20-stamp Clay stroke recorded into ONE `TopologyDelta` and
  reverted was bit-identical to the pre-stroke surface and validated.

## What Changes

- **`brush::apply_to_dynamic`** (`include/clay/brush/stroke.h`), the fifth
  consumer of `resolve_stroke`, beside `apply_to_mesh` and `apply_to_multires`.
  Each stamp brings its radius and strength; Grab centres on the first stamp;
  Snakehook centres on the vertex it drags, revalidated every stamp and re-found
  at the previous stamp position when a collapse retired it; the mask is placed
  once and gates every verb; the cavity and group estimators are wired from
  `MeshStrokeOptions`; `orient_alpha_by_stamp` turns the alpha. Every stamp goes
  through `DynamicSculptor::stamp`, so the per-verb remesh schedule
  (`default_timing`: Grab AFTER, Clay BEFORE, Snakehook BEFORE+AFTER) runs on
  every stamp exactly as it does for a single one.
- **Layer is refused**, not mapped: `apply_to_dynamic` applies nothing and
  returns 0 for `MeshBrush::Layer`, and both bindings refuse it with the reason
  `dynamic_offers` records.
- **`MeshStrokeOptions::defer_normals` is refused on this path.** The adaptive
  sculptor has no normal deferral; accepting the flag and ignoring it is the
  "accepted and ignored" failure the brush-engine spec forbids.
- **`DynamicSculptor::nearest_vertex` becomes public**, so the stroke re-finds a
  dead anchor with the SAME estimator the sculptor seeds its walk with, rather
  than a second copy in `brush`.
- **C ABI (0.118.0 -> 0.119.0, after #617 took 0.118.0):** `clay_dynamic_sculptor_apply_stroke` and
  `clay_dynamic_sculptor_apply_preset`. Both take `clay_stroke_sample_full` (the
  azimuth-carrying packing), the session frame the handle declares, a topology
  descriptor, an optional mask, `orient_alpha_by_stamp`, and accumulate a
  `clay_dynamic_stamp_report` over the stroke. No new struct.
- **pyclay:** `DynamicSculptor.apply_stroke` and `DynamicSculptor.apply_preset`,
  mapped by the gate's existing `clay_dynamic_sculptor_` prefix.
- **Swift:** the smoke (`tests/swift/smoke.swift`) drives
  `clay_dynamic_sculptor_apply_stroke`, including the Layer refusal.
- **Docs:** `docs/07` §8b gains the stroke; `docs/05` the two entry points; the
  ROADMAP rows that name the gap are closed.

## What this does not do

- **It does not change `apply_to_mesh`'s Grab.** Anchored on the first stamp, a
  pull-out Grab reaches 41% of the drag on the fixed mesh on `main`, against 66%
  for a following centre, because the region is re-gathered around a point the
  surface has left. Whether that is right is a question about the fixed path
  with its own goldens; this change makes the adaptive surface agree with it and
  names it for a follow-up issue.
- **It does not carry a topology undo record across the C ABI.**
  `clay_dynamic_sculptor_stamp` passes none either; the ABI has no handle for a
  `TopologyDelta`. The C++ entry point takes one and the header says so.
- **It does not widen the flat-packed mesh and multires stroke calls.** They
  still take `count*5` floats and so cannot carry an azimuth; that is
  `carry-the-barrel-and-the-speed-response`'s stated limit and unchanged here.
- **No device latency case.** The per-stamp path the device gate measures is
  untouched; the new call adds a loop around it.

## What building it found

**The sum of a stroke's stamps exposed a defect in the single stamp.** The
first "a stroke's summary equals its stamps" run did not agree on the last
stamp's revisions. `DynamicSculptor::stamp_impl`'s reached-nothing exit (a
fully masked or empty gather) returned the revisions it read on ENTRY, so a
Clay stamp whose BEFORE remesh changed the topology reported that nothing had,
and it published the remesh counters a second time on top of `stamp()`'s
publish. The AFTER remesh was folded in field by field on both exits, and
neither carried `hit_budget` (one also dropped `relaxed`), so a Grab whose
remesh stopped at its budget reported a converged region. Fixed in its own
commit with two regressions in `test_dynamic_sculpt.cpp`; restoring the old
`dynamic_sculpt.cpp` fails both (stale revisions, doubled counters, missing
`hit_budget`). A host re-uploading on a revision change was the one who paid.

**The re-find is caught by one test, not two.** Task 3.10 expected both
Snakehook tests to fail with the re-find broken. Mutated to keep the dead
anchor's last position, the detail-4 test fails (reach **15.1%**, 11 anchor
deaths counted in the hand loop, surfaces differ) and so does the Snakehook row
of "a stroke equals its stamps" (surfaces differ, 2653 vs 3021 splits) — but
the detail-8 reach test still passes: at detail 8 deaths are rare (the table
above: 2 in 13 stamps at spacing 0.25) and a stroke that loses its anchor late
still clears 90%. The test was kept for what it asserts — that the anchor rule
beats a following loop — and the death precondition lives only in the detail-4
test, which counts it.

**The other mutations, each caught:** Grab centred on the cursor fails the Grab
row of the equality test; dropping `defer_normals` from the refusal fails its
test (surface, record and count); skipping no fully masked stamp fails the mask
test with topology on. **Dropping Layer from the stroke-level refusal was
invisible to the surface**, because `DynamicSculptor::stamp` refuses Layer per
stamp as well: the stroke's refusal is only observable in what it would have
set up first. The Layer and `defer_normals` tests now pass a cavity estimator
and assert the sculptor's automask inputs stayed empty, and the Layer mutation
fails that check.

**The first cut of `apply_to_dynamic` scored 18**, over the backend target.
Moved the summary reset, the frozen-centre early-out and the per-stamp settings
(anchor revalidation, alpha orientation) into helpers: `apply_to_dynamic` 12,
`dynamic_stamp_settings` 4, `dynamic_snakehook_centre` 2, the rest 1 (clang-tidy
`readability-function-cognitive-complexity`). For reference, on `main` and
untouched here, `apply_to_mesh` scores 19 and `apply_to_multires` 21. In the C
ABI: `clay_dynamic_sculptor_apply_preset` 13, `clay_dynamic_sculptor_stamp` 11,
`read_dynamic_topology` 11,
`clay_dynamic_sculptor_apply_stroke` 9, `apply_dynamic_stroke` 6.

**`check_c_abi.py` needed to learn the shared report fill.** Its bounded-fill
rule looks for a `write_desc` bounded by the caller's `struct_size` at each
entry point; moving the stamp's report fill into `write_dynamic_report` so the
stroke and the stamp cannot fill it two ways meant naming that helper in
`BOUNDED_FILLS`, as `write_preflight` already is.

**A stroke checks its report BEFORE running, a stamp still checks it after.**
A short report on a stroke would otherwise leave a whole stroke applied under a
call that returned an error. `clay_dynamic_sculptor_stamp` keeps its existing
order; changing it is not this change's to make.

**Review found three claims and one gap the tests did not cover.** The
1.001x cited beside the call priced the host's loop, taken before the call
existed; re-measured against the call it is 1.004x (above). "Keeping a dead
anchor loses 82–85%" held only for the rows where the anchor died 6–14 times;
the rows with 1–3 deaths reach 41–88%, and the headers, docs and spec now say
both. And no test could fail if the stroke summary kept the LAST stamp's
`hit_budget` instead of OR-ing it: a new case ends on a stamp that reaches
nothing, and that mutation fails it (the bounds mutation fails it and the
equality test). The short-report C case asserted any failure; it now asserts
`CLAY_ERROR_INVALID_ARGUMENT`.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `brush-engine`: stamps apply to an adaptive surface. The estimator
  requirement is REMOVED and re-ADDED under a new name, because its scenario
  "The adaptive path has no stroke resolver of its own" states the gap this
  closes and a MODIFIED block may not drop a scenario (openspec 1.12.0 refuses
  it); its text and two other scenarios carry over unchanged.
- `dynamic-topology`: a stroke keeps the per-verb remesh schedule on every stamp
  and keeps a dragged anchor alive across it.
- `c-abi`: a host can stroke an adaptive surface across the ABI.
- `python-bindings`: an adaptive stroke is reachable from Python.

## Impact

- `include/clay/brush/stroke.h`, `src/brush/stroke.cpp` — the consumer, sharing
  `mesh_stamp_settings`, `mesh_mask_gate` and `mesh_automask_inputs` with the
  two existing ones.
- `include/clay/mesh/dynamic_sculpt.h` — `nearest_vertex` public.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `tools/check_c_abi.py` if its
  mirror needs the new calls, the three version lines.
- `bindings/python/pyclay_module.cpp`, `tests/swift/smoke.swift`.
- Tests: `tests/unit/test_dynamic_stroke.cpp` (new, joins `_rest`),
  `tests/unit/test_c_dynamic_topology.cpp`, a pyclay test.
- `docs/05-claycore-library.md`, `docs/07-brushes-and-features.md`,
  `openspec/ROADMAP.md`.
