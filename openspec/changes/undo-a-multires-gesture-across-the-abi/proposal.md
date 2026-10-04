## Why

**A C host cannot put a multiresolution gesture into its undo stack** (#671).
The engine records both kinds of gesture — `mesh::MultiresDelta` for the base
(cage positions, a level's own coefficients) and `mesh::SculptLayerDelta` for a
sculpt pass — and `session::History` replays them, but neither crossed the ABI.
`bindings/c/clay.h` at 0.124.0 said so twice: `clay_multires_sculptor_apply_stroke`
("the ABI does not yet carry that record") and the sculpt-layer stroke
transaction ("THE RECORD ITSELF DOES NOT CROSS THIS ABI YET"). Both pointed a
host at pyclay, which does not expose either record either.

A `clay_multires` is a standalone handle that no `scene::Layer` owns, so
`clay_document_undo` does not reach it. `clay_multires_sculpt_layer_stroke_cancel`
is exact, but only before commit. After commit a host had an entry count and
nothing else; its only undo was a `clay_multires_serialize` snapshot of the whole
hierarchy per gesture, priced against a 128 / 256 MiB history budget on iPad
(ClaySpaceIOS design D7).

The fixed mesh (`clay_mesh_deltas`) and the adaptive surface (`clay_dynamic_delta`)
already have host-held per-gesture records. Multires was the only sculptable
representation without one.

## What changes

- **`clay_multires_delta`**, an opaque host-owned record holding both halves of
  a gesture, with `_create`, `_destroy`, `_clear`, `_stats_get`, `_levels`,
  `_revert`, `_apply`, `_serialize` and `_deserialize` — the `clay_dynamic_delta`
  shape, so a host keeps one pattern.
- **Three ways in**: `clay_multires_sculptor_stamp_recorded`,
  `clay_multires_sculptor_apply_stroke_recorded` (new entry points; the shipped
  calls are unchanged), and `clay_multires_sculpt_layer_stroke_commit_into`.
- **A binding** the C++ records do not have: the hierarchy's
  `structure_revision` and a per-process origin at capture. A replay onto any
  other hierarchy or structure is `CLAY_ERROR_SNAPSHOT_MISMATCH` with nothing
  written.
- C++: `mesh::MultiresGesture` (new, `include/clay/mesh/multires_gesture.h`)
  owns the binding and the envelope; `MultiresDelta` and `SculptLayerDelta`
  gain `matches`, split counts and `encoded_size`; `brush::apply_to_multires`
  gains a trailing, defaulted `SculptLayerDelta*`.
- The two "does not cross" paragraphs in `clay.h` now name the new calls.
- ABI 0.124.0 -> 0.125.0, additive.

## What measuring found

All numbers from this branch, cpu-only Release, Apple arm64, through the C ABI
only (`tests/unit/test_c_multires_delta.cpp` and a throwaway probe).

### 1. The C++ records accept a hierarchy they do not describe

`MultiresDelta::revert` and `SculptLayerDelta::revert` refuse only a surface
whose counts cannot hold the entries. Two cases pass that check and are wrong:

| pairing | counts match | C++ record alone | `clay_multires_delta` |
|---|---|---|---|
| a twin built from the same cage, same levels | yes | **writes into it** | `SNAPSHOT_MISMATCH`, nothing written |
| this hierarchy after `remove_highest_level` + `add_level` | yes | **writes into it** | `SNAPSHOT_MISMATCH`, nothing written |

With the binding disabled (`bound_to` returning true), 8 assertions in the
refusal case fail: both replays succeed and both surfaces change.

### 2. The plain sculptor's record was half a record

`brush::apply_to_multires` passed only the base record to
`MultiresSculptor::stamp`. On a hierarchy with an active pass the plain sculptor
writes the pass, so a recorded stroke came back **empty** and undid nothing.
With the layer half forced to null, 7 of 14 cases fail (31 assertions).

### 3. A reverted pass marked no patch

A base write marks its patches as it lands (`set_detail` -> `mark_patches`). A
pass write only queues blocks for recomposition, and patches are marked when a
level is next evaluated. After a layer-half revert
`clay_multires_dirty_block_count` was **0** while the evaluated revision had
moved — a host re-copying its dirty blocks after an undo redrew nothing. The
replay now evaluates the display level before it returns.

### 4. What a record costs

16x16 triangle cage, seven unrecorded strokes first, then one recorded nine-stamp
Draw stroke of radius 0.3 at the top level:

| levels | top-level vertices | entries | encoded | resident | whole-hierarchy snapshot |
|---|---|---|---|---|---|
| 2 | 6,273 | 1,109 | 35,544 | 79,172 | 95,816 |
| 3 | 24,833 | 4,445 | 142,296 | 315,812 | 317,104 |
| 4 | 98,817 | 17,759 | 568,344 | 1,262,012 | 1,202,160 |

The encoding is 37-47% of the snapshot here. **The resident record is not
smaller than the snapshot on this fixture**: it carries slot maps, about 2.2x
its encoding. The difference is scaling — a snapshot carries all detail sculpted
so far, a record one gesture — and the encoding is what a host spills to.

One stamp and forty stamps on the same spot record the same entry count and the
same encoded bytes (6x6 cage, level 2).

## What building it found

- The plan said "revert/apply refused on a changed level structure". Counts
  cannot say that: removing the top level and adding it back leaves every count
  equal. `structure_revision` can, and it is process-wide, which is why the
  binding also carries a per-process origin.
- The plan expected the layer half to need only `commit_into`. The plain
  sculptor writes the active pass too (finding 2), so the recorded sculptor calls
  carry both halves, and continuing a record is refused only while a
  **different** pass is active — a base write joins either half.
- The plan did not expect the dirty-patch gap (finding 3).
- The issue proposed `size_t clay_multires_delta_serialize(...)`. It follows
  `clay_dynamic_delta_serialize` instead (`clay_result`, `size_t* count`, size
  query, `BUFFER_TOO_SMALL`), so a short buffer is retryable and distinguishable
  from a malformed call.
- Level add/remove records (the issue's optional item) are not in this change.
  Under the binding chosen here a level change invalidates earlier records, so a
  host undoing past one restores from its snapshot, as it does today.

## Impact

- C ABI: 0.124.0 -> 0.125.0, twelve new entry points and one descriptor
  (`clay_multires_delta_stats`); no shipped signature or layout changes.
- C++: additive (`MultiresGesture`, `matches`, `encoded_size`, split counts, a
  defaulted trailing parameter).
- No format change: the multires and scene formats are untouched; the record's
  own envelope is versioned (`CMRD`, version 1).
- pyclay does not gain the record in this change; `check_binding_parity.py`
  compares pyclay to C, and pyclay does not expose `MultiresDelta` either.
