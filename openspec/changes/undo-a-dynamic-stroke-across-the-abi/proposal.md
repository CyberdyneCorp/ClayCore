## Why

**A host holding a `clay_dynamic_surface` has no undo.** `mesh::TopologyDelta`
records a stroke as one sparse, reversible step and `session::History` replays
it (`include/clay/session/history.h:304`, `src/session/history.cpp:273,507`),
but `grep -c TopologyDelta bindings/c/clay.h` returns 0 and
`clay_dynamic_sculptor_stamp` passes `nullptr` for the record. The adaptive
surface lives outside `clay_document` by design, so the document's history
cannot reach it either. A host's only undo today is
`clay_dynamic_surface_serialize` per stroke: 3,195,040 bytes on a
12,288-face sphere, against a 1,233,636-byte encoded record for the same stroke.

`bindings/python/tests/test_dyntopo.py` says undo was held back "on a known open
defect". That defect was the free-list cycle, fixed by `aae921fe` (#437, v0.78.0)
two minutes after that comment was committed. The comment is stale. Measuring
the gap turned up three other problems, listed below. Each one breaks an undo a
host would build on the C++ calls as they are.

All numbers below come from `origin/main` at `aafeccb6`, cpu-only Release, Apple
arm64. The probes are out-of-tree doctest files linked against `libclaycore.a`.
Fixture: the cube-sphere from `test_dynamic_history.cpp`, a 30-stamp Draw stroke,
`BrushRelative` detail, and topology defaults (so `relax_after_remesh` is on).
The timings come from single runs and are only illustrative. Nothing below gates on them.

## What measuring found

### 1. Undo leaves the sculptor's index stale, and the next stroke goes wrong

`TopologyDelta::revert` restores the surface. The sculptor's `DynamicBvh` is not
told. Faces the stroke deleted come back as live faces in no chunk. Faces it
created stay in their chunks as dead entries. No chunk is marked dirty.

| fixture | live faces missing from the index | dead entries | dirty chunks | same stroke again equals the first |
|---|---|---|---|---|
| cube_sphere(24), 6,912 faces, revert only | 200 | 1,390 | **0** | **no** |
| cube_sphere(64), 49,152 faces, revert only | 2,638 | 180 | **0** | **no** |
| either, revert + `rebuild_index` | 0 | 0 | **0** | yes |
| either, revert + incremental erase/insert/refit | 0 | 0 | 34 / 56 | yes |

A fresh sculptor reproduces the first stroke bit-exactly. A sculptor kept across
the undo does not. After the second stroke, 2 and 18 live faces are still
missing from its index. A host drawing from the dirty-chunk stream after an undo
would see holes. It is never told to redraw anything.

`rebuild_index` repairs the index, but it is the wrong tool for this job. It
**clears** the dirty set, because the build assumes the host has not read the
chunks yet. It also renumbers the chunks (88 -> 64, 527 -> 512). And it costs
O(surface):

| faces | 30-stamp stroke | `revert` | incremental index | `rebuild_index` |
|---|---|---|---|---|
| 49,152 | 54.09 ms | 0.315 ms | 0.106 ms | 39.18 ms |
| 196,608 | 56.43 ms | 0.942 ms | 0.295 ms | 189.04 ms |
| 786,432 | 111.73 ms | 4.008 ms | 2.067 ms | **895.61 ms** |

**So an undo through the ABI keeps the index in step by itself, incrementally,
and does not require `rebuild_index`.**

### 2. Redo, and sometimes undo, restores the wrong normals

The relax pass in `remesh_local.cpp` does three things in order:

1. It notes and syncs each vertex around its position write.
2. It calls `refresh_normals` over the incident faces.
3. It never notes those faces or their other vertices, and never syncs again.

So the record's `after` normals are stale. For some elements its `before` is
missing altogether.

Measured immediately after capture, the record's `after` state disagrees with
the live surface on 346 vertex normals and 669 face normals. Connectivity,
positions and generations all agree.

| stroke (n, radius, strength, detail) | undo: vertex normals wrong | redo: vertex normals wrong |
|---|---|---|
| 32, 0.3, 0.3, 8 | 0 | 346 |
| 32, 0.3, 0.0, 8 | 0 | 760 |
| 16, 0.3, 0.0, 16 | 0 | 2,785 |
| 48, 0.15, 0.05, 12 | 0 | 16 |
| 32, 0.3, -0.3, 8 | 0 | 572 |
| 24, 0.4, 0.6, 6 | **14** | 251 |
| 8 strokes on cube_sphere(32), reversed then redone | **2,910** (sum) | 2,675 (sum) |
| any of the above, `relax_after_remesh = false` | 0 | 0 |

Positions and indices were exact in every row. This breaks the live
dynamic-topology requirement that a revert is bit-exact. It went unseen because
`test_dynamic_history.cpp`'s `same_mesh` compares positions and indices, and
`test_topology_delta.cpp` drives the operators without the relax pass. A host
reads the wrong normals through `clay_dynamic_surface_copy_chunk` and
`clay_dynamic_surface_to_mesh`. **It is fixed in this change, with a regression
test.**

### 3. A record replayed onto the wrong state corrupts silently

`TopologyDelta::revert` always returns `true`.

- **Undoing stroke 0 while strokes 1 through 7 are still applied**: `validate`
  fails with "half-edge 1205 names a dead edge". Stroke 6 fails the same way.
- **Two strokes on opposite hemispheres are not independent.** The later stroke
  reused 27 vertex, 161 half-edge, 80 edge and 54 face slots that the earlier
  one had freed. Reverting the earlier one under it failed `validate`.

Undo that skips a step is therefore unsound even when the strokes are far apart
in space, so replay needs a guard. One candidate is a read-only content check:
compare each recorded element's liveness, generation and content against the
record's end state. It measured **0.051 ms over 13,759 elements** and caught
both cases above. It is still rejected in `design.md`. It cannot prove soundness
when an unrecorded edit in between rewrote an element the record does not name.
The guard chosen instead is an O(1) surface mark, exact for last-in first-out
replay.

### 4. What a record costs, as a count

On arm64, `sizeof(ElementDelta)` is 124 / 116 / 44 / 68 bytes (vertex /
half-edge / edge / face). The encoding is fixed-width: a 24-byte header, then
122 / 114 / 42 / 66 bytes per entry. **For one stroke on cube_sphere(32), with
1,215 V / 6,952 H / 3,349 E / 2,306 F entries, that formula gives 1,233,636, and
`encode().size()` returned exactly 1,233,636.** `bytes()`
(capacities plus the slot maps) returned 1,884,384 against a 1,261,256-byte
payload, so it depends on the allocator. It is right for a budget and wrong for
an exact assertion.

| cube_sphere(32), 12,288 faces | entries | V | F | `bytes()` | encoded |
|---|---|---|---|---|---|
| stroke 0 | 8,935 | 832 | 1,546 | 1,539,952 | 801,878 |
| stroke 1 | 6,968 | 832 | 1,548 | 943,232 | 631,400 |
| stroke 4 | 6,621 | 831 | 1,511 | 937,680 | 602,682 |
| stroke 7 | 3,754 | 559 | 1,004 | 539,552 | 339,548 |

Undo does not shrink the surface. The slot pools never compact. After three
strokes were undone, the face slots stood at 14,964, up from 12,288. Surface
`bytes` went from 3,637,376 to 6,684,928, and the extra shows up as
`clay_dynamic_surface_stats.dead_slots`. The serialized size was unchanged
(3,195,040), but the bytes were not identical. Every probe that compared
`encode()` after a revert differed, while the live-slot fingerprint, `to_mesh`
and `validate` all matched. The difference is in the slots the pool retired. The exact comparison for "undo restored the surface" is
therefore `to_mesh` plus `validate`, not `serialize`.

## What changes

- **The C ABI, 0.117.0 -> 0.118.0.** An opaque `clay_dynamic_delta` record and
  its calls:
  - `_create`, `_destroy` and `_clear`;
  - `_stats_get`, which reports entry counts per kind, `encoded_bytes` (exact:
    `56 + 122V + 114H + 42E + 66F`) and `resident_bytes` (what the record holds
    in memory);
  - `_revert` and `_apply`, which take the sculptor;
  - `_serialize` (with the size query) and `_deserialize`.

  Capture uses a new `clay_dynamic_sculptor_stamp_recorded`, which is
  `clay_dynamic_sculptor_stamp` plus a record argument. The shipped stamp is
  unchanged.
- **Replay runs through the sculptor.** Replay first checks the surface's
  `{lineage, epoch}` mark against the record's end mark, and refuses a mismatch
  with `CLAY_ERROR_SNAPSHOT_MISMATCH` before writing anything. Replaying onto
  the state the record already describes succeeds and changes nothing. Otherwise replay restores the surface, then
  erases, inserts and refits exactly the chunks the record names, and marks
  them dirty.
- **The relax pass records the normals it rewrites.** It notes the incident
  faces and their vertices before the write and syncs them after
  `refresh_normals`. Undo and redo then match the live surface bit-for-bit,
  normals included.
- **pyclay:** a `TopologyDelta` class, and `DynamicSculptor.stamp(...,
  record=)`. **Swift:** `tests/swift/smoke.swift` gains a capture/undo/redo
  round-trip.
- The stale "UNDO/REDO IS DELIBERATELY ABSENT" paragraph in `test_dyntopo.py` is
  replaced by the parity tests.

## What this deliberately does NOT do

- **No new layer kind and no change to the container format.** Both were decided
  before this proposal. The host keeps the surface and its records beside its
  document.
- **No engine-side history for the dynamic surface.** The host owns the
  sequence: which record is next, how deep it goes, and when to drop one. The
  engine owns only the proof that a record matches the surface.
  `clay_document`'s history is not extended to a handle it does not hold.
- **No selective, out-of-order undo.** Measurement 3 shows that spatially
  disjoint strokes still share slots. The mark refuses any replay that is not
  last-in first-out, and the header says so. It does not reorder anything.
- **No compaction after undo.** Dead slots stay. Compaction would renumber the
  handles the record is keyed on, which would invalidate every record the host
  still holds.
- **The C++ `session::History` path is not changed.** It resolves a
  `DynamicSurface` rather than a sculptor, so the stale-index hazard from
  measurement 1 still applies to a C++ caller who keeps a sculptor across
  `History::undo`. No ABI caller can reach that path. It is recorded in
  `design.md` as a follow-up.

## What building it found

- **The capture could not be a `stamp` overload.** Every existing call site
  spells `stamp(..., nullptr)` for the record, and a second pointer overload
  makes each of those calls ambiguous. It is `DynamicSculptor::stamp_recorded`,
  which returns `std::optional` and gives nullopt, having stamped nothing, when
  the capture guard refuses.
- **A per-surface epoch counter would have been unsound, so the epoch comes
  from a process-wide counter.** A counter that restarts from the restored
  epoch after an undo hands the undone stroke's epochs out again. The next
  stamp then lands on the undone record's `after` mark, and that record would
  "match" a surface it no longer describes. Epochs are therefore drawn from a
  process-wide counter that never rewinds; `set_mark` is the only way back to
  an old one. A test checks this: mutating `bump_*` to `epoch + 1` fails
  `an undone stroke's epochs are never handed out again`.
- **The decoder's per-entry byte constants were 8 bytes short.** They counted
  one handle where every entry carries two (122 / 114 / 42 / 66, not 114 / 106 /
  34 / 58). A valid record was unaffected, but the pre-allocation bound against
  a hostile count was looser than stated. `encoded_size()` is now computed from
  those constants, a `static_assert` pins them to the documented widths, and the
  C test holds `encoded_bytes` equal to what serialize writes.
- **The relax fix did not grow the record.** The expected growth did not show
  up: with the fix and without it, the stroke from `test_dyntopo.py` recorded the
  same 4,410 V / 25,880 H / 12,652 E / 8,531 F entries (4,582,826 encoded bytes)
  on a 34,655-face sphere, and the same 15,379 / 91,084 / 45,013 / 30,227
  (16,145,398) on 138,162 faces. The vertices the relax pass moves sit around
  remesh operations whose faces and corners were already noted. The fix changes
  the values recorded, not which elements are recorded.
- **The audit (task 3.2) found no writer that skips a bump.** Every writer of
  the four pools bumps a revision after its last write, with no return in
  between: split / collapse / flip in `topology_ops.cpp`, the relax pass,
  `write_positions`, `write_colors`, and `TopologyDelta::revert` / `apply`.
  `clay_dynamic_sculptor_trim` compacts only the chunk arena, never the pools.
  No C ABI or pyclay entry point writes the pools directly.
- **A second sculptor over the same surface is not kept in step.** This is not
  new: a stamp through one sculptor already leaves the other's index stale. The
  header now says so beside the replay calls.
- **Mutation checks, each run against a freshly rebuilt binary:**
  - reverting `remesh_local.cpp` to `origin/main` fails the normals assertions
    in both new test files;
  - dropping the erase and re-insert steps, or only the erase before the
    restore, fails the chunk-stream, repeat-stroke and index-coverage cases;
  - dropping the replay guard fails every refusal case, and `validate` fails
    after the out-of-order revert;
  - dropping the capture guard fails the capture-refusal cases.
- **Cognitive complexity** (clang-tidy): `replay` 4, `reindex_recorded_faces` 8,
  `refit_around_moved_vertices` 14, `RecordedGesture::decode` 9, `guard` 5,
  `clay_dynamic_sculptor_stamp_recorded` 10, `read_dynamic_topology` 11.
  `relax_region` is 39, down from 45 on `origin/main`. It was already above the
  target and is not made worse.
- **Swift:** the new checks pass when `smoke.swift` is built against the
  cpu-only `libclaycore.a` (432 of 433; the one failure is the Metal-backend
  registration check, which a cpu-only library cannot pass). The xcframework
  run was not repeated.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`: 9 new entry points, 1 new struct
  and the version lines (`CMakeLists.txt`, `clay.h`, `pyproject.toml`).
- `include/clay/mesh/dynamic_surface.h`: a restorable `SurfaceMark`
  (`{lineage, epoch}`), with the epoch advanced by the existing `bump_*` calls.
- `include/clay/mesh/topology_delta.h`, `src/mesh/topology_delta.cpp`: a
  `RecordedGesture` (a delta plus its before and after marks, with its own
  encoding around the unchanged `CTDL` bytes), and a const view of the face and
  vertex entries for index maintenance.
- `include/clay/mesh/dynamic_sculpt.h`, `src/mesh/dynamic_sculpt.cpp`:
  `DynamicSculptor::replay(const RecordedGesture&, direction)` and
  `DynamicSculptor::stamp_recorded`, which captures into a `RecordedGesture`.
- `src/mesh/remesh_local.cpp`: the relax pass notes and syncs its normals.
- `bindings/python/pyclay_module.cpp`, `tools/check_binding_parity.py`
  (`TopologyDelta` -> `clay_dynamic_delta_`), `tests/swift/smoke.swift`.
- `docs/05`, the library reference: the adaptive-surface undo section.
