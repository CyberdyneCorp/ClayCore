## Context

`MultiresDelta` and `SculptLayerDelta` are value records: each entry is a
(level, vertex) with its value before and after the gesture, coalesced. Replay
writes absolute values, so records on disjoint vertices commute and a replay is
idempotent. Neither record knows which hierarchy it came from.

## Decisions

### D1. One record type holding both halves

A host's undo stack has one slot per gesture and no owner to dispatch on. The
plain sculptor writes whichever half the stack's active layer selects, so a host
recording "this stroke" cannot know in advance which C++ record it needs.
`MultiresGesture` holds both; an empty half costs nothing and encodes as a zero
length. `session::History` keeps its two step kinds — the reason given there
(`step_bytes` per kind) is about the document's accounting, which this record
is outside.

### D2. Bound to `structure_revision` and a per-process origin

The binding has to refuse a twin hierarchy and a relevelled one (proposal,
finding 1). Options considered:

| binding | twin refused | relevel refused | survives relevel-and-back | survives decode of own bytes |
|---|---|---|---|---|
| entry counts (the C++ records today) | no | no | yes | yes |
| a fingerprint of the level structure | no | yes | yes | yes |
| the handle pointer | yes | no | yes | no |
| `structure_revision` + process origin (chosen) | yes | yes | **no** | **no** |

A fingerprint accepts the twin, which the acceptance criteria refuse. The handle
pointer misses relevelling and can be reused after a free. `structure_revision`
is drawn from one process-wide counter, moves on every renumbering, and never
on a sculpt or a cache release — exactly the "same numbering" predicate
`MultiresSculptor::bind` already relies on. It restarts per process, so the
record also carries one random origin per process (`SurfaceMark::fresh().lineage`,
the adaptive surface's per-process seed) and a record from another process
replays onto nothing.

The cost is the two "no" cells: a record does not survive a level change even
one later undone, nor a snapshot restore into a new handle. That is the same
lifetime rule `clay_dynamic_delta` states ("a record never outlives its surface
handle"), and the alternative failure — coefficients written into renumbered
vertices — is silent.

### D3. Replay through the hierarchy, not a sculptor

`clay_dynamic_delta` replays through the sculptor because the sculptor owns a
chunked index the replay must maintain. A multires sculptor reads the
hierarchy's own per-level chunk table and level mesh, which `set_detail` /
`set_base_position` update. Measured: the same stroke stamped through the same
sculptor after a revert reproduces the first stroke's checksums and every
level's positions bit-identically.

### D4. Validate everything before writing anything

`replayable` checks the binding, then `MultiresDelta::matches` and
`SculptLayerDelta::matches` (split out of their `revert`/`apply`), before either
half writes. A record whose pass was removed refuses with the base half
untouched.

### D5. Evaluate the display level after a pass replay

Pass content is recomposed lazily; without an evaluation the dirty-patch list
stays empty (proposal, finding 3). Evaluating the display level is the work the
host's next block copy would do anyway. A base-only replay does not evaluate.

### D6. Refusal order and continuation

Recorded calls return every `INVALID_ARGUMENT` the unrecorded call would —
including a malformed report size, checked before stamping — and only then
`SNAPSHOT_MISMATCH` for a record that may not be continued. A record may be
continued while bound to this structure and while no **different** pass is
active. `commit_into` requires an empty record and leaves the stroke open when
refused.

### D7. Serialization envelope

`"CMRD"`, version, origin, structure revision, then each half as a 64-bit length
and that record's own encoding. Each half's decoder keeps its own validation;
the envelope adds the length bound before any allocation, the exact-size check
(`encoded_size() == length`), the no-trailing-bytes check, and "non-empty iff
bound". `encoded_bytes = 40 + (16 + 32d + 28c) + (24 + 32ld + 16lm)`, each
bracket only for a non-empty half.

## Rejected

- **A new argument on the shipped stamp/stroke/commit calls** — breaks every
  host compiled against them.
- **Merging into a non-empty record at `commit_into`** — `MultiresDelta` has no
  merge, and choosing which gestures form one step is the host's decision.
- **Refusing a replay while a layer stroke is open** — the composition hold is
  also taken by hosts driving the plain sculptor, so it is not proof of an open
  transaction. Stated in the header instead.
