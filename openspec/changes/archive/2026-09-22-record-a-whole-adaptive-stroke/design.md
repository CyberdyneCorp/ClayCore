## Context

`mesh::RecordedGesture` (#617) is the replayable record: a `TopologyDelta` plus
the surface marks at both ends. Its capture contract is three calls —
`can_capture_on` (an empty record binds anywhere; a non-empty one only where it
left the surface), `begin_capture` (binds `before` when empty) and
`end_capture` (moves `after`; an empty record re-binds `before = after`). The
only caller today is `DynamicSculptor::stamp_recorded`, which wraps one
`stamp`.

`brush::apply_to_dynamic` (#619) is the stroke: one `DynamicSculptor::stamp`
per resolved stamp, with Grab centred on the first stamp and Snakehook on a
revalidated, re-found vertex. It already threads a raw `TopologyDelta*` into
every stamp. The C calls reach it through `apply_dynamic_stroke` in
`clay_c.cpp`, which passes `nullptr` for the record; pyclay does the same.

## Goals / Non-Goals

**Goals:**
- A whole adaptive stroke, with the stroke's semantics, recorded as ONE guarded
  undo step from C++, the C ABI and pyclay.
- The surface a recorded stroke produces is bit-identical to the unrecorded
  stroke's.
- A mismatch or refusal applies nothing and leaves the record untouched.

**Non-Goals:**
- Changing `apply_to_dynamic`, `clay_dynamic_sculptor_apply_stroke` or
  `_apply_preset`.
- Changing replay semantics, compaction, or the record's encoding.
- A device latency case (see proposal, "What this does not do").

## Decisions

### D1. A C++ sibling, not an overload and not a migrated parameter

`brush::apply_to_dynamic_recorded(..., mesh::RecordedGesture& record, const
MeshStrokeOptions& options = {}, mesh::DynamicStampResult* summary = nullptr)
-> std::optional<std::size_t>`.

- **Overload rejected.** `apply_to_dynamic(..., nullptr, ...)` is spelled at the
  C, pyclay and test call sites; a `RecordedGesture*` overload makes each
  ambiguous. A `RecordedGesture&` overload is unambiguous but returns a
  different type under the same name, and `stamp` / `stamp_recorded` already
  set the house convention of a separate name.
- **Migration rejected.** Changing the `TopologyDelta*` parameter to
  `RecordedGesture*` needs a way to report a mismatch; `std::size_t` has none
  (0 already means "nothing changed" and "refused"). Every caller would change
  for a case only recorded callers can hit. The C++ API is not ABI-frozen, but
  the only non-null caller of the raw parameter is a test, and the raw form
  stays meaningful exactly as `DynamicSculptor::stamp`'s does: a C++ owner that
  sequences its own history.
- The header of `apply_to_dynamic` gains one line: `record` is the UNGUARDED
  delta; use `apply_to_dynamic_recorded` for one `DynamicSculptor::replay`
  accepts.

### D2. The mark is checked once, before the first stamp

Implementation is the prototype of the proposal:

```
reset summary (as every refusal does)
if refused(stamps, verb, options): return 0            // record untouched
if !record.can_capture_on(surface): return nullopt     // nothing stamped
record.begin_capture(surface)
applied = apply_to_dynamic(..., &record.delta_mutable(), ...)
record.end_capture(surface)
return applied
```

Refusals of the stroke's own arguments come BEFORE the mark check, so a Layer
stroke into a stale record reports the malformed call (0 in C++,
`CLAY_ERROR_INVALID_ARGUMENT` in C) rather than a retryable mismatch. They also
come before `begin_capture`, so a refused call does not even re-bind an empty
record. `dynamic_stroke_refused` is reused so the two functions cannot disagree
on what is refused.

Evidence that no mismatch can arise at k>0 is proposal section 1: 0 refusals
over 97 stamps on seven fixtures (all six verbs, the anchor-death Snakehook,
relax on), the mark moving exactly when a stamp reports a change, and a mutation
that injects one unrecorded stamp making the same counter fire at k=3. A
per-stamp check is therefore not added — it would be a check that cannot fire.

**Contract if that ever changed** (a future stroke that writes the surface
outside `stamp`): the per-stamp check would have to precede the stamp and the
call would have to report the stamps already applied as captured, since they
are in the delta. That is recorded here so a later change does not silently
return nullopt after partial application.

### D3. Accumulation is allowed

A stroke into a non-empty record whose end is the current surface continues it,
exactly as `stamp_recorded` does, and one revert undoes everything captured
(proposal section 2: dab + Clay stroke + Grab stroke, one revert to pristine).
A host that wants one step per stroke clears the record first; `clear()` keeps
capacity.

A stroke that changed nothing into an EMPTY record leaves it empty and
re-binds its `before` to the current state, which is `end_capture`'s documented
behaviour for a stamp that recorded nothing. Into a non-empty record it changes
nothing, marks included (proposal section 4, last row).

### D4. C ABI: two `_recorded` entry points, NULL record = unrecorded

```c
clay_result clay_dynamic_sculptor_apply_stroke_recorded(
    clay_dynamic_sculptor* sculptor, const clay_stroke_sample_full* samples,
    size_t sample_count, const clay_stroke_preset* preset,
    const clay_mesh_brush_desc* brush, const clay_dynamic_topology_desc* topology,
    const clay_mask* mask, int32_t orient_alpha_by_stamp,
    clay_dynamic_delta* record, size_t* out_applied,
    clay_dynamic_stamp_report* out_report);

clay_result clay_dynamic_sculptor_apply_preset_recorded(
    clay_dynamic_sculptor* sculptor, const clay_stroke_sample_full* samples,
    size_t sample_count, const clay_brush_preset* preset, const float* alpha,
    int32_t alpha_width, int32_t alpha_height,
    const clay_dynamic_topology_desc* topology, const clay_mask* mask,
    int32_t orient_alpha_by_stamp, clay_dynamic_delta* record,
    size_t* out_applied, clay_dynamic_stamp_report* out_report);
```

`record` sits where `_stamp_recorded` puts it: after the inputs, before the
outputs. The shipped `_apply_stroke` / `_apply_preset` become one-line
forwards with `record = NULL`, as `clay_dynamic_sculptor_stamp` forwards to
`_stamp_recorded`, so there is one implementation.

Order of checks, unchanged for everything that already exists and with the mark
LAST: null sculptor -> brush / preset / samples -> report size -> Layer ->
topology -> mask -> **record mark** (`CLAY_ERROR_SNAPSHOT_MISMATCH`, the same
message family as `_stamp_recorded`) -> stroke. `apply_dynamic_stroke` gains a
`clay_dynamic_delta*` and calls the C++ sibling when it is non-null.
`*out_applied` is 0 and the report is not written on a mismatch, as on every
other failure.

### D5. pyclay: `record=` keyword

`DynamicSculptor.apply_stroke(..., record=None)` and `.apply_preset(...,
record=None)`, taking the existing `TopologyDelta` (the Python name of
`RecordedGesture`), GIL released across the capture as `stamp` does. A
mismatch raises `ValueError` with `stamp`'s wording adapted ("nothing was
applied"). The parity gate reads members, not keywords; the pairing with the
`_recorded` C calls is held by tests on both sides and a comment beside
`"TopologyDelta.stats"` in `check_binding_parity.py`, as #617 did for `stamp`.

### D6. Header cost statement

Beside the new C calls: "recording costs what recording the same stamps with
`clay_dynamic_sculptor_stamp_recorded` costs — 1.046x / 1.065x the unrecorded
stroke at 27,648 / 110,592 faces (14 Draw stamps, median of 61 / 21, Release),
against 1.049x / 1.061x for the per-stamp recorded loop; a reused record saves
under 1%."

## Risks / Trade-offs

- **[A future writer inside the stroke loop breaks D2's premise]** -> the
  equivalence test (recorded stroke == per-stamp `stamp_recorded` loop, with a
  `REQUIRE` on every `stamp_recorded`) fails the moment a non-`stamp` write
  moves the mark mid-stroke, and D2 records the contract that would then apply.
- **[Two record kinds in C++ confuse callers]** -> `apply_to_dynamic`'s header
  names the raw record unguarded and points at the sibling; the C ABI and
  pyclay only ever expose the guarded one.
- **[Docs from #619 now contradict the header]** -> corrected in this change,
  listed per file in tasks.
