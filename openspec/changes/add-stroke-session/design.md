## Context
`brush::StrokeTransaction` accumulates samples and re-resolves the whole path on every append, handing back the stamps not returned before. It is C++ only. The C ABI has whole-path stroke calls, so a host forwarding a live Pencil gesture in pieces restarts spacing, taper, steady and jitter at every call. ClaySpaceIOS works around this by re-implementing the spacing phase and the lazy-mouse state on the host, with the engine's steady set to 0, and cannot get the end taper right because no piece knows it is the last (#670, ClaySpaceIOS design D6).

The issue left one decision to the engine: what a consumer does with a stamp a later append revises.

## Decisions

### Consumers apply settled stamps only
A stamp is applied once no later append can change what a consumer reads from it (position, radius, strength, deposit, rotation), and not before. The alternative the issue offered, applying every new stamp and letting the preview settle at lift, is not available to most consumers: a voxel brush, a mask stroke, a mesh vertex move and an SDF node cannot be un-applied by a later append. Applying a stamp that is still tapering would leave a narrowing in the middle of a stroke that later grew past it.

The settle rule follows from reading `resolve_stroke`:
- At least two samples. A lone sample has no direction yet, and its azimuth has not been through the interpolation every later resolve applies to it.
- The station lies on the received path (`d <= length`). The count's `1e-3` epsilon admits one station just past the end, clamped to the endpoint for now; it moves when the path goes on.
- Outside the end taper at the current length. `along = d / length` only falls as the path grows, so a station that has left the zone stays out.
- No start taper. `taper_start` is a fraction of the whole stroke, so every station eventually falls inside it as the stroke grows. Nothing of a start-tapered stroke settles before the end.

Everything else is causal: steady is a lag over samples already received, pressure, tilt, azimuth and velocity are interpolated within a received segment, and jitter hashes the station index. `along` is excluded from "settled". It moves on every append and no consumer reads it.

Under the rule, a consumer fed the settled stamps as they settle applies exactly the stamps of the whole path, in order. So a session gesture is bit-identical to the whole-path call by construction, and the tests assert that for every consumer.

### The mesh consumers become gestures
`apply_to_mesh`, `apply_to_multires` and `apply_to_dynamic` hold per-stroke state between stamps: the first stamp (a grab moves by the motion since the stroke began), a grab's carried region, a snakehook's anchor, the multires level record (`begin_stroke`), and the deferred-normal flag. A stroke per call would reset all of it. `test_c_stroke_session.cpp` shows the difference: a grab fed batch by batch to the whole-path call lands somewhere else.

So each consumer is now a class (`MeshStrokeGesture`, `MultiresStrokeGesture`, `DynamicStrokeGesture`) with `apply(stamps)` and `finish()`. The whole-path functions construct one, feed it once and finish it. They are the same code, which removes the risk of the two drifting. The per-stamp body moved into the gesture unchanged, setup included: the multires call keeps returning without restoring the deferral when no level is bound, as it always has.

### One consumer per session, bound by the first call
A gesture has one target and one brush, fixed at pointer-down. The first consumer call binds the session: it decodes the descriptors and copies an SDF item. Later calls must name the same target with the same handle pointers, scalars and descriptors, or they are refused with `CLAY_ERROR_INVALID_ARGUMENT`. `clay_mesh_brush_desc` has two padding holes, so a byte comparison would refuse a host that rebuilt an identical descriptor on the stack. Every call decodes its descriptors instead, and a later call's decoded `MeshBrushSettings`, `DynamicTopologySettings` and frame are compared with the bind's field by field, floats by their bits. Each struct is destructured with a structured binding that names every member, so adding a member breaks the build at the comparison. The gesture keeps using what the bind decoded.

### What a gesture holds open between calls
- SDF, voxel and mask consumers hold the owning document's undo group open from the bind to the close, so the gesture is one undo step without host bracketing. Brackets nest, so a host that brackets anyway is unaffected.
- Mesh consumers hold the carried region and the deferral. Deferred normals are flushed once, into the closing call's record.
- The dynamic consumer checks the record before it takes stamps from the session. A refused call (`CLAY_ERROR_SNAPSHOT_MISMATCH`) therefore loses no stamps: once the host repairs its history, the next call applies them.

The session borrows its target. Destroying a session with an open gesture closes it, which touches the target, so the header says to destroy the session first.

### Frames
A sculptor that declares a world frame takes its stroke in world space and carries the samples and the preset's lengths into the mesh's space before resolving. A sink for such a sculptor keeps a second transaction in that space, fed the same samples converted the same way. Its stamps are then the floats the whole-path call resolves, not world stamps converted after the fact. The cost is a second resolve, and only for a framed sculptor.

### Resolve cost
Every append re-resolves the whole path. `sample_path` searched for each station's segment from the start of the path, so a resolve cost stations × samples, and a session paid that on every append. The search now resumes from the previous station's segment, which finds the same segment because stations and the cumulative arc length both only increase. The cost per append becomes linear in the path. The proposal's measurements give the numbers.

## Not done
- `clay_multires_sculpt_layer_stroke_*` has no whole-path stroke call to be equivalent to. It takes one brush descriptor per stamp, so a session consumer for it would first need that call. This is follow-up work alongside #671.
- The brush-preset entry points (`*_apply_preset`) carry their own stroke preset. A session already has one, and the two would disagree.
- pyclay gets the session, not the consumers. See `C_ONLY_FOLLOW_UPS` in `tools/check_binding_parity.py`.
