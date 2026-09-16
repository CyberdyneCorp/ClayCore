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

| path | detail | spacing | stamps | anchor died | reach, re-found at last anchor position | reach, re-found at cursor | reach, never re-found |
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
a stroke that keeps a dead anchor's last position stops pulling (15–18% reach).
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
- **C ABI (0.117.0 -> 0.118.0):** `clay_dynamic_sculptor_apply_stroke` and
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
