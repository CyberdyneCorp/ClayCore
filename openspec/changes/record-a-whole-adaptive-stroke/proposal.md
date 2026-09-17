## Why

Two changes landed on the adaptive surface within a day of each other and do not
meet:

- `undo-a-dynamic-stroke-across-the-abi` (#617, ABI 0.118.0) made a
  REPLAYABLE record: `mesh::RecordedGesture`, a `TopologyDelta` plus the
  `{lineage, epoch}` surface marks at both ends, captured by
  `DynamicSculptor::stamp_recorded` / `clay_dynamic_sculptor_stamp_recorded` and
  replayed last-in-first-out through the sculptor, refused with
  `CLAY_ERROR_SNAPSHOT_MISMATCH` on a wrong state.
- `stroke-an-adaptive-surface` (#619, ABI 0.119.0) made a whole STROKE:
  `brush::apply_to_dynamic`, `clay_dynamic_sculptor_apply_stroke` /
  `_apply_preset`, `DynamicSculptor.apply_stroke` / `.apply_preset`.

Counted on `origin/main` at `a60d8c16` (ABI 0.119.0):

| surface | whole stroke | replayable record |
|---|---|---|
| C++ | `apply_to_dynamic(..., mesh::TopologyDelta* record, ...)` | **a raw delta, no marks** — cannot go through `DynamicSculptor::replay` |
| C ABI | `clay_dynamic_sculptor_apply_stroke`, `_apply_preset` | **none** — the calls take no `clay_dynamic_delta` |
| pyclay | `DynamicSculptor.apply_stroke`, `.apply_preset` | **none** — no `record=` |

So a host that wants the stroke's MEANING and an undo step has to pick one. The
only recorded path across the ABI is a host loop of
`clay_stroke_resolve_full` plus `clay_dynamic_sculptor_stamp_recorded`, and the
headers #619 wrote say exactly that. That loop is not the stroke, and the
difference is measured below rather than argued.

### A recorded host loop is a different stroke

Probe: a scratch binary linked against this tree's Release `libclaycore.a`, a
`cube_sphere(16)` built through the C ABI, 24 samples along a line at z=1,
preset radius 0.3 / spacing 0.25 / taper_end 0.3, brush strength 0.4, topology
brush-relative at detail 6 (relax on). "C stroke" is the unrecorded
`clay_dynamic_sculptor_apply_stroke`. "C loop" is the host's only recorded
option: resolve, then one `clay_dynamic_sculptor_stamp_recorded` per stamp,
centred on the stamp, direction = motion since the last stamp. "C++ recorded
stroke" is the prototype below on the same mesh and the same resolved stamps.

| verb | stamps | C++ recorded stroke == C stroke (positions, bit-exact) | C loop == C stroke (positions) | encoded bytes, C loop | encoded bytes, recorded stroke |
|---|---|---|---|---|---|
| Draw | 6 | yes | **yes** | 664,260 | 664,260 |
| Snakehook | 6 | yes | **no** | 1,107,948 | 1,205,836 |
| Grab | 6 | yes | **no** | 1,069,644 | 448,280 |

Draw is not a drag, so the loop and the stroke coincide, down to the record's
byte count. Grab and Snakehook are drags: the stroke centres Grab on the first
stamp and Snakehook on the vertex it drags (re-found when the remesher retires
it); the loop centres both on the cursor. The loop's record is a faithful undo of
a DIFFERENT surface — #619 measured that surface at 42% of a Snakehook pull-out
against 96%. A host today has to choose between the right stroke and an undo
step.

## What Changes

- **C++:** `brush::apply_to_dynamic_recorded(sculptor, stamps, verb, settings,
  topology, mask, mesh::RecordedGesture& record, options, summary)` returning
  `std::optional<std::size_t>` — nullopt, with nothing stamped and the record
  untouched, when the record is non-empty and the surface is no longer where it
  left it. A SIBLING, for the reason `stamp_recorded` is one: an overload on a
  pointer would make every `apply_to_dynamic(..., nullptr, ...)` call site
  ambiguous, and a migrated parameter would need a new return type at every call
  site to report the mismatch. `apply_to_dynamic`'s `TopologyDelta*` stays,
  documented as the UNGUARDED record (the same raw form `DynamicSculptor::stamp`
  takes).
- **C ABI (0.119.0 -> 0.120.0):** `clay_dynamic_sculptor_apply_stroke_recorded`
  and `clay_dynamic_sculptor_apply_preset_recorded`: the shipped calls'
  arguments plus a `clay_dynamic_delta* record`, mirroring `_stamp` /
  `_stamp_recorded`. `record` NULL behaves exactly as the unrecorded call.
  A mismatched record is `CLAY_ERROR_SNAPSHOT_MISMATCH`, applied nothing, record
  untouched, checked AFTER every `CLAY_ERROR_INVALID_ARGUMENT` refusal so a
  malformed call is never reported as retryable. The shipped
  `clay_dynamic_sculptor_apply_stroke` / `_apply_preset` are unchanged.
- **pyclay:** `record=` (a `TopologyDelta`) on `DynamicSculptor.apply_stroke` and
  `.apply_preset`; a mismatch raises `ValueError` and applies nothing, as
  `stamp(record=)` does.
- **Swift smoke:** a recorded stroke, reverted and re-applied, with
  `clay_dynamic_surface_validate` at both ends.
- **Docs:** correct what #619 wrote — that a stroke can only be recorded per
  stamp across the ABI — in `clay.h` beside `clay_dynamic_sculptor_apply_stroke`,
  `docs/05` "Stroking an adaptive surface", `docs/07` "A whole stroke",
  `pyclay`'s `apply_stroke` docstring, and the `stroke-an-adaptive-surface`
  c-abi delta (unarchived, so its sentence is corrected in place).

## Measured before building

All numbers from scratch probes linked against this tree's Release
`libclaycore.a` (`-O3`, arm64), none committed. The prototype is four lines and
is what the design adopts:

```cpp
if (!record.can_capture_on(sc.surface())) return std::nullopt;
record.begin_capture(sc.surface());
std::size_t applied = brush::apply_to_dynamic(sc, stamps, verb, settings, topo, mask,
                                              &record.delta_mutable(), options, summary);
record.end_capture(sc.surface());
```

### 1. Mismatch is checked once, and cannot arise at stamp k>0

A per-stamp loop of `stamp_recorded` with the stroke's own rules (Grab on the
first stamp, Snakehook on its re-found vertex) was run beside the prototype on
every fixture of section 3, counting `stamp_recorded` refusals at k>0 and
whether the surface mark moved exactly when a stamp reported a change.

| fixture | stamps | refusals at k>0 | mark moved, stamp unchanged | stamp changed, mark still | loop surface == stroke surface |
|---|---|---|---|---|---|
| all 7 rows of section 3 | 97 | **0** | 0 | 0 | yes, all 7 |
| MUTATION: one unrecorded stamp injected before k=3 | 6 | **3** (first at k=3) | — | — | — |

The mutation proves the counter can fire. Structurally: inside the call the
only writer to the surface is `DynamicSculptor::stamp`, each into the same
delta; `nearest_vertex` and `vertex()` are const, and `set_automask_inputs`
writes the sculptor, not the surface. The host already serializes calls on a
sculptor. So the mark is checked ONCE, before the first stamp, and a mismatch
refuses the whole stroke with nothing applied; a per-stamp check would be a check
that cannot fire, and is not added. No partial stroke can be left unreported.

### 2. A stroke accumulates into a non-empty record

One `RecordedGesture`: a `stamp_recorded` Draw dab, then a recorded Clay stroke,
then a recorded Grab stroke, on `cube_sphere(16)`.

| after | encoded bytes |
|---|---|
| the dab | 707,358 |
| + Clay stroke | 1,263,002 |
| + Grab stroke | 1,647,616 |

ONE revert restored the pristine surface exactly (export, stored normals over
live elements, `validate`), and one apply restored the end. Accumulation is
allowed, as `stamp_recorded` allows it; the record grows by what each stroke
newly reached.

### 3. Undo and redo are exact, and recording changes nothing

`cube_sphere(16)` and the shaped stroke of `test_dynamic_stroke.cpp` (taper,
jitter, pressure ramp, 6 stamps) per verb at strength 0.5, default topology
(`relax_after_remesh` ON); plus that file's anchor-death Snakehook
(`cube_sphere(24)`, detail 4, spacing 0.05). "exact" = `to_mesh` positions,
normals and indices bit-identical AND stored vertex and face normals and
positions over every live element bit-identical, `validate` ok, and no live face
missing from the sculptor's index after the replay.

| fixture | stamps | splits | collapses | anchor deaths | undo exact | redo exact | == unrecorded `apply_to_dynamic` | encoded, stroke | encoded, per-stamp loop |
|---|---|---|---|---|---|---|---|---|---|
| Draw | 6 | 1,447 | 713 | — | yes | yes | yes | 851,178 | 851,178 |
| Clay | 6 | 1,454 | 707 | — | yes | yes | yes | 853,306 | 853,306 |
| Smooth | 6 | 1,526 | 731 | — | yes | yes | yes | 904,378 | 904,378 |
| Flatten | 6 | 1,525 | 732 | — | yes | yes | yes | 899,058 | 899,058 |
| Grab | 6 | 1,845 | 1,097 | — | yes | yes | yes | 870,094 | 870,094 |
| Snakehook | 6 | 3,021 | 1,851 | 2 | yes | yes | yes | 1,488,796 | 1,488,796 |
| Snakehook, anchor deaths | 61 | 309 | 167 | **11** | yes | yes | yes | 221,506 | 221,506 |

"== unrecorded" is the surface (as above), the applied count, the split count
and the moved-vertex count. The per-stamp loop is the C++ loop with the stroke's
rules, so its bytes equal the stroke's exactly; the ABI host loop in "Why" does
not apply those rules and its bytes differ for the two drags, for the reason
given there.

### 4. Refusals leave the record untouched

Into a record already holding a Draw stroke (record state = encoded size and
both marks):

| refusal | returns | record | surface / revisions |
|---|---|---|---|
| `MeshBrush::Layer` | 0 | unchanged | unchanged |
| `defer_normals` | 0 | unchanged | unchanged |
| empty stamps | 0 | unchanged | unchanged |
| an unrecorded stamp in between | nullopt | unchanged | unchanged (no stamp ran) |
| record from another surface | nullopt | unchanged | the other surface unchanged |
| every stamp misses the surface (non-empty record) | 0 | unchanged | mark unchanged |

A short `clay_dynamic_stamp_report` is already refused before the stroke runs
(`check_dynamic_report`, gated by "c dynamic stroke: a short report is refused
before the stroke is applied"); the recorded calls keep that order, and the
tests add the record's stats to that assertion.

### 5. Cost

Draw, 14 stamps (preset radius 0.3, spacing 0.125), topology default, each run
on a fresh surface, arms interleaved, Release, median. "reused" is one record
`clear()`ed between runs, which keeps capacity.

| surface | runs | splits (all arms) | encoded bytes (all recorded arms) | unrecorded stroke | recorded stroke, fresh record | recorded stroke, reused record | `stamp_recorded` loop, same stamps |
|---|---|---|---|---|---|---|---|
| `cube_sphere(48)`, 27,648 faces | 61 | 826 | 1,160,592 | 71.803 ms | 75.070 (**1.046x**) | 75.054 (1.045x) | 75.324 (1.049x) |
| `cube_sphere(96)`, 110,592 faces | 21 | 419 | 4,210,594 | 97.912 ms | 104.289 (**1.065x**) | 103.866 (1.061x) | 103.879 (1.061x) |

An earlier run of the same probe without the loop arm read 1.053x / 1.046x at 48
and 1.065x / 1.055x at 96, so the band is about 1.05-1.07x. The overhead is the
delta capture inside `DynamicSculptor::stamp`, which the per-stamp recorded loop
pays identically (1.049x, 1.061x): recording a whole stroke costs what recording
its stamps costs, and a reused record saves under 1%. The equal split and byte
counts across arms are the assertion; the clock is the header's statement.

## What this does not do

- **It does not add a per-stamp mark check.** Section 1 shows it cannot fire.
- **It does not change `apply_to_dynamic` or the shipped C calls.** The raw
  `TopologyDelta*` stays for C++ callers that own their sequencing, as
  `DynamicSculptor::stamp` keeps its own.
- **It does not change what a replay promises.** Last in, first out; dead slots
  are not compacted; serialize is not byte-identical after an undo — all as
  #617 states.
- **It is not a latency change.** Recording costs what `stamp_recorded` costs per
  stamp (section 5); the stroke adds two mark writes.
- **It does not add a device latency case.** `check_device_coverage.py` matches
  `clay_dynamic_sculptor_stamp(` exactly, and neither `_stamp_recorded` nor
  #619's `_apply_stroke` is in its verb list; this change is consistent with
  that and records it rather than widening the gate here.

## What building it found

The design held: `apply_to_dynamic_recorded` is the four-line prototype behind
the stroke's own refusals, and the C and pyclay halves are thin. What building
it found was about the tests, and one of them would have shipped inert.

- **Undo/redo exactness cannot see an unbound start.** Mutation (a) of tasks 3.6,
  deleting `begin_capture`, passed all 489 assertions of the first draft of the
  C++ tests: an empty record then keeps `before = {0, 0}`, a revert restores the
  surface bit-exactly and stamps it with that zero mark, and the apply accepts
  the zero mark as the start. Every export, normal and index check agreed. The
  host-visible damage is elsewhere — the surface is left at a mark no OLDER
  record ends at, so the undo stack below the stroke is stranded. The test that
  catches it records a dab, then a stroke, reverts both last in first out, and
  checks the stroke's `before()` is the dab's end: 4 assertions fail under the
  mutation, 0 without it.
- **(b) dropping the mark check** fails 10 assertions in the refusal test (both
  mismatch rows: the stroke ran, the record grew, revisions advanced, and the
  replay no longer refused). **(c) checking the mark after the stroke** fails 6
  (surface and revisions moved, summary written, record changed).
- **C ABI mutations:** ignoring the record fails 11 assertions over 3 cases;
  returning `CLAY_OK` on a mismatch fails 3; checking the mark before the Layer
  refusal fails 1 — the Layer row. The short-report row cannot fail under that
  mutation, because the report size is checked in the entry point before
  `apply_dynamic_stroke` runs, so that half of design D4's order is structural.
- **pyclay mutation** (the record never reaches the engine): all 5 new pytest
  cases fail.
- **A first-draft assertion was wrong, not the code:** comparing the recorded
  stroke's `before()` with the per-stamp loop's across two surfaces fails,
  because every surface has its own lineage. The byte equality (task 3.3) is the
  comparison that means something.
- **The parity gate lists neither the Python stroke methods nor the C stroke
  calls by name**; it passed with 766 capabilities before and after. The
  `record=` pairing is held by `test_dyntopo.py` and `test_c_dynamic_delta.cpp`,
  and the gate's comment beside `TopologyDelta.stats` now says so.
- **Cognitive complexity** (clang-tidy): `apply_to_dynamic_recorded` 3,
  `apply_dynamic_stroke` 8, `clay_dynamic_sculptor_apply_stroke_recorded` 8,
  `clay_dynamic_sculptor_apply_preset_recorded` 12, pyclay `stroke_dynamic` 1
  (clang-tidy's figure; 3 by hand).

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `brush-engine`: an adaptive stroke can be captured into a replayable gesture,
  checked once.
- `c-abi`: recorded siblings of the adaptive stroke and preset calls.
- `python-bindings`: `record=` on the adaptive stroke and preset methods.

## Impact

- `include/clay/brush/stroke.h`, `src/brush/stroke.cpp` — the sibling.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp` — two entry points; ABI
  0.119.0 -> 0.120.0 in `CMakeLists.txt`, `clay.h`, `pyproject.toml`.
- `bindings/python/pyclay_module.cpp`, `bindings/python/tests/test_dyntopo.py`.
- `tests/unit/test_dynamic_stroke.cpp`, `tests/unit/test_c_dynamic_topology.cpp`
  (or `test_c_dynamic_delta.cpp`), `tests/swift/smoke.swift`.
- `tools/check_c_abi.py` if its FFI mirror lists the dynamic calls.
- `docs/05-claycore-library.md`, `docs/07-brushes-and-features.md`,
  `openspec/ROADMAP.md`, `openspec/changes/stroke-an-adaptive-surface/specs/c-abi/spec.md`.
