## Why
A live-input host gets Pencil samples in batches whose size depends on the device: 240 Hz coalesced touches, split by frame pacing and main-thread load. The artist's stroke should not depend on that batching. `brush::StrokeTransaction` already handles it: it accumulates samples and re-resolves the path, so forty samples in one batch and in five batches of eight give the same stamps. But it is C++ only. Every C stroke call takes a whole path, so a host sending a gesture in pieces restarts spacing, taper, steady and jitter on each call. ClaySpaceIOS (#670, its design D6) works around this by carrying the spacing phase and the lazy-mouse state across joins on the host, with the engine's steady set to 0. It cannot get the end taper right, because no piece knows it is the last.

## What Changes
- C ABI 0.125.0 -> 0.126.0. `clay_stroke_tx`: `_begin(preset)`, `_append(samples_full, count, &new_stamps, &revised_from)`, `_end`, `_stamps` (size query), `_status_get` (`clay_stroke_tx_status`, with a leading `struct_size`), `_destroy`.
- One consumer per representation, each applying the session's settled and not-yet-applied stamps: `clay_layer_apply_stroke_tx`, `clay_voxel_apply_stroke_tx`, `clay_mask_apply_stroke_tx`, `clay_mesh_sculptor_apply_stroke_tx`, `clay_dynamic_sculptor_apply_stroke_tx`, `clay_multires_sculptor_apply_stroke_tx`. The first call binds the session to one target and one brush. The call after `_end` applies the rest and closes the gesture.
- **The revised-stamp rule (the engine's decision, asked for in the issue): a consumer applies a stamp only once it is settled**, meaning no later append can change its position, radius, strength, deposit or rotation. Settled means two or more samples, a station on the received path, outside the end taper at the current length, and no start taper in the preset. Under this rule a session gesture is bit-identical to the whole-path call. The cost is that ink trails the pen: by up to one spacing, by the end-taper fraction of the stroke when there is an end taper, and by the whole stroke when there is a start taper. `clay_stroke_tx_stamps` always gives the stroke as it now stands, so a preview can be drawn from it.
- `brush::StrokeTransaction` gains `settled()`, `revised_from()` and `finish()`, plus `brush::StampCursor` and `resolve_stroke_settled`. `apply_to_grid` gains `first_index`, because the voxel dither seeds each stamp with its index in the stroke.
- `apply_to_mesh`, `apply_to_multires` and `apply_to_dynamic[_recorded]` become one gesture fed once (`MeshStrokeGesture`, `MultiresStrokeGesture`, `DynamicStrokeGesture`). The whole-path calls and the session calls run the same code.
- `resolve_stroke` searches for each station's segment from where the previous station's search stopped, not from the start of the path. The segments found are the same, and the cost goes from stations × samples to linear.
- pyclay: `StrokeTransaction` (`append`, `end`, `stamps`, `status`).

## Measurements
The cost of `append` on a 240 Hz stroke at 0.25 world units per second, with radius 0.02, spacing 0.15 and an end taper. Measured through pyclay, so each figure includes about 2 µs of binding overhead. Median of 15 runs, Apple M-series, Release.

| stroke length | stamps | last append, before | last append, after | whole `resolve`, before | whole `resolve`, after |
|---|---|---|---|---|---|
| 1 s (240 samples) | 50 | 4.6 µs | 2.5 µs | 6.3 µs | 4.0 µs |
| 5 s (1200) | 250 | 60.4 µs | 9.7 µs | 63.4 µs | 13.1 µs |
| 10 s (2400) | 502 | 216.3 µs | 18.9 µs | 219.7 µs | 22.8 µs |

"Before" is this change without the segment-search fix. Its cost per append grew with the square of the stroke's length. A whole 10-second gesture fed 8 samples at a time costs 2.98 ms in total across 300 appends, against 23.41 ms before the fix.

## What building it found
- **The issue's second option does not work for most consumers.** "Let the preview settle at lift" means applying a stamp that a later append revises, and a voxel brush, a mask stroke, a vertex move or an authored SDF node cannot be revised. Applying the provisional stamps of an end-tapered stroke leaves a narrowing wherever the pen paused.
- **A start taper cannot settle early.** It is a fraction of the whole stroke, so every station eventually falls inside it. The rule says so rather than approximating.
- **The count's epsilon admits a station past the end of the path.** When `length / step` lands just under an integer (and is at least 1), the last station sits beyond the received path and is clamped to its endpoint. It moves as soon as the path goes on. The first version of the rule settled it. A targeted test (`stroke settle: the station the count's epsilon admits past the end waits`) is the only one that catches this, since the general property test never lands within 1e-3 of an integer.
- **A lone sample is not settled either.** Its direction is +X by convention and its azimuth has not been through `lerp_angle`, which a two-sample path puts it through (`atan2(sin a, cos a)` is not bit-equal to `a`).
- **A stroke per call is a different grab.** Feeding the batches to `clay_mesh_sculptor_apply_stroke` one at a time re-gathers the carried region at every call, and the surface lands somewhere else. The test asserts this, so the gesture refactor is shown to be necessary as well as sufficient.
- **Descriptors cannot be compared by bytes.** `clay_mesh_brush_desc` has two padding holes. Later calls decode their descriptors again and compare the decoded settings, topology and frame field by field. The comparison destructures every member, so a field appended to `MeshBrushSettings`, `DynamicTopologySettings` or `math::Transform` fails to compile until it is compared. The first version compared nothing after the bind while the header promised a refusal; a host that changed strength mid-gesture got `CLAY_OK` and the old brush.
- **A refused recorded call must not consume stamps.** The adaptive sink checks the record before it takes stamps from the session, so `CLAY_ERROR_SNAPSHOT_MISMATCH` loses nothing.
- **The resolver was quadratic**, which nothing noticed while each stroke was resolved once. A session resolves on every append, which is how it showed up.
- Mutation check. Each of these defects, introduced on purpose, fails at least one new test: dropping the end-taper condition, the past-the-end condition or the start-taper hold; settling nothing until the end; restarting the voxel dither seed per call; restarting the mesh gesture per call; closing the layer undo group per call; ignoring a sculptor's frame for the samples.

## Impact
Additive: a new opaque handle, a status descriptor and twelve entry points. The whole-path calls give the same results as before, with the same refusals and reports, and the existing suite passes unchanged. No format change.
