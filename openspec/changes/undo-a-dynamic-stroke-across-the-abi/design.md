# Design

## The ABI

```c
typedef struct clay_dynamic_delta clay_dynamic_delta;

clay_dynamic_delta* clay_dynamic_delta_create(void);
void clay_dynamic_delta_destroy(clay_dynamic_delta* delta);
/* Empties the record and unbinds it. KEEPS its capacity, so a host that reuses one
 * record per stroke allocates on the first stroke only. Destroy releases it. */
clay_result clay_dynamic_delta_clear(clay_dynamic_delta* delta);

typedef struct clay_dynamic_delta_stats {
    uint32_t struct_size;   /* = sizeof(clay_dynamic_delta_stats); required */
    uint64_t vertices;      /* entries per kind: one per element the gesture reached */
    uint64_t halfedges;
    uint64_t edges;
    uint64_t faces;
    /* EXACT and platform-independent: what clay_dynamic_delta_serialize writes. */
    uint64_t encoded_bytes;
    /* What the record holds in memory, capacities included. Allocator-dependent.
     * This is the number a host budgets against. */
    uint64_t resident_bytes;
} clay_dynamic_delta_stats;
clay_result clay_dynamic_delta_stats_get(const clay_dynamic_delta* delta,
                                         clay_dynamic_delta_stats* out_stats);

/* clay_dynamic_sculptor_stamp, plus the record the stamp accumulates into.
 * `record` NULL behaves exactly like clay_dynamic_sculptor_stamp. */
clay_result clay_dynamic_sculptor_stamp_recorded(clay_dynamic_sculptor* sculptor,
                                                 const clay_mesh_brush_desc* brush,
                                                 const clay_dynamic_topology_desc* topology,
                                                 const clay_mask* mask,
                                                 clay_dynamic_delta* record,
                                                 clay_dynamic_stamp_report* out_report);

/* Undo and redo. Both run through the SCULPTOR, because the sculptor owns the
 * index and the chunk stream that have to follow the surface. */
clay_result clay_dynamic_delta_revert(const clay_dynamic_delta* delta,
                                      clay_dynamic_sculptor* sculptor);
clay_result clay_dynamic_delta_apply(const clay_dynamic_delta* delta,
                                     clay_dynamic_sculptor* sculptor);

/* Size-query pattern. For spilling a record out of memory within the lifetime of
 * the surface handle it was captured on. It is NOT crash recovery. */
clay_result clay_dynamic_delta_serialize(const clay_dynamic_delta* delta, uint8_t* out_data,
                                         size_t* count);
clay_result clay_dynamic_delta_deserialize(const uint8_t* data, size_t size,
                                           clay_dynamic_delta** out_delta);
```

There are nine entry points and one struct, modelled on `clay_mesh_deltas`: an
opaque record, replay through the sculptor, and the same verbs. The additions:

- **A new stamp entry point**, rather than a new argument to the existing one.
  Changing the signature of `clay_dynamic_sculptor_stamp` would break every host.
  A handle cannot go into a descriptor field either, because descriptors carry
  values, not borrowed pointers.
- **A stats struct**, rather than `clay_mesh_deltas_vertex_count`'s single count.
  A record has four kinds of entry, and the goal is a byte cost.
- **Serialization.** A host over its undo budget can either drop the oldest record
  or spill it to disk. Without serialization only the first option exists, and
  `encoded_bytes` would describe something the host could not act on.

Rejected: **`clay_dynamic_sculptor_set_recording(sculptor, delta)`**. The sculptor
would hold a borrowed pointer to a record the host can destroy while it is still
set. The header rules forbid a borrowed pointer that can dangle with no check.

Rejected: **an undo stack owned by the sculptor.** The host already decides depth,
grouping, and whether an undo crosses into its own document edits. The engine
cannot see any of those. The engine owns one thing only: proving that a record
matches the surface.

## Replay: guard, restore, keep the index in step

`DynamicSculptor::replay(const RecordedGesture&, Direction)` does three things,
in order, and writes nothing before the first one passes.

### 1. The guard is an exact state mark, not a content comparison

Measurement 3 in the proposal settles that the guard is required. Two strokes on
opposite hemispheres shared 322 slots, and replaying out of order failed
`validate`. The open question was what the guard should compare.

- **Content comparison was rejected.** For each recorded element it compares
  liveness, generation and value against the record's end state. It is
  O(record); it measured 0.051 ms over 13,759 elements. It catches every overlap
  and every slot reuse it can see. But it cannot prove the replay sound when an
  **unrecorded** edit in between rewrote an element the record does not name,
  while a named element still points at it. A passing check would then not
  guarantee a valid surface.
- **A surface mark was chosen.** `DynamicSurface` carries a `SurfaceMark`, which
  is a pair `{lineage, epoch}`.
  - `lineage` is process-unique per surface instance. It is drawn from an atomic
    counter mixed with a per-process random seed, so a record from another
    process or another surface never matches.
  - `epoch` advances inside `bump_topology`, `bump_geometry` and
    `bump_attributes`. Those are the calls every mutating operator already makes,
    because the host's re-upload depends on them.
  - A record stores the mark before its first recorded stamp and after its last.
    - **Revert** requires `surface.mark == record.after`, then sets
      `surface.mark = record.before`.
    - **Apply** requires `surface.mark == record.before`, then sets
      `surface.mark = record.after`.
  - The check is O(1), and it is exact for the contract promised: records are
    replayed strictly last-in first-out, with no unrecorded change in between.

A surface already at the target mark returns `CLAY_OK`, writes nothing and bumps
no revision. That keeps the C++ record's idempotence: reverting twice is
reverting once. Any other mark is `CLAY_ERROR_SNAPSHOT_MISMATCH`. That code
already means "a record handed a state it was not taken against, nothing
applied, retry with the right state works". `CLAY_ERROR_INVALID_ARGUMENT` would
tell the host the call was malformed, which is the opposite reading.

Capture is guarded the same way. A `stamp_recorded` into a bound, non-empty
record whose `after` mark differs from the surface is refused with
`CLAY_ERROR_SNAPSHOT_MISMATCH` before stamping. That happens when the record came
from another surface, or when an unrecorded stamp or a replay came in between.
Continuing would coalesce two unrelated histories into one step.

A stamp that changed nothing does not advance the epoch, because it bumps no
revision. So a host's miss-everything dab does not invalidate its history.

A `DynamicSurface` copied in C++ keeps the source's lineage. The C ABI cannot
copy a surface. `clay_dynamic_surface_deserialize` and `_from_mesh` draw a new
lineage, so a record never outlives the handle it was captured on. The header
says this, and says that serialization is for spilling a record, not for
recovering it after a crash.

### 2. Restore

`TopologyDelta::revert` / `apply` run unchanged, including the free-list rebuild
that `aae921fe` added.

### 3. Keep the index and the chunk stream in step, incrementally

`TopologyDelta` gains a const view of its face and vertex entries. From the
record alone, with `target` meaning the end being restored to:

- For every face entry, `bvh.erase(slot)` runs **before** the restore. It is a
  no-op on a slot that is not indexed. It has to come first because
  `DynamicBvh::insert` returns early when a slot already has a leaf. A slot whose
  generation changed inside the gesture would otherwise keep its stale entry.
- Every face that exists at `target` is re-inserted after the restore. Insertion
  marks its chunk topology-dirty and refits the chunk's ancestors.
- Vertex entries whose position differs between the two ends mark their incident
  faces for `update_many`. Once the relax fix below lands, those faces are all
  face entries already. The pass stays as a cheap assertion target.

The costs are in the proposal: 0.106 ms at 49k faces and 2.067 ms at 786k,
against 39 ms and 896 ms for `rebuild_index`. The prototype kept every live face
indexed, marked 34 and 56 chunks dirty, and a repeat of the same stroke was
bit-exact to the first.

**`rebuild_index` is not required after an undo.** It is still correct to call.
It is also still advisory, driven by `index_quality`: re-insertion places a face
by centroid, so the partition can decay the way a long stroke already makes it
decay. The header states two consequences. A rebuild **clears the dirty set** and
**renumbers chunks** (measured 88 -> 64). A host that rebuilds after an undo must
therefore re-upload every chunk, whatever the dirty list says.

### How this composes with the rest of the surface API

| | after a replay |
|---|---|
| `clay_dynamic_surface_revision` | all three revisions advance, as `TopologyDelta::revert` already bumps them; a no-op replay advances none |
| dirty chunks | the chunks holding faces the record names are dirty; draining them reconstructs `clay_dynamic_surface_to_mesh` |
| chunk count | may grow, since a re-insert can split a leaf; hosts already handle this after a stamp |
| `clay_dynamic_surface_stats.dead_slots` | does not return to its pre-stroke value, because pools never compact |
| `clay_dynamic_surface_serialize` | NOT byte-identical to the pre-stroke bytes: retired slots carry bumped generations. `to_mesh` and `validate` are the exact observables |
| world frame, automask sources | untouched; neither is surface state |
| `clay_dynamic_sculptor_memory_ledger` | does not count records; the host owns them and reads `resident_bytes` |

## The relax pass records what it rewrites

Today, in `src/mesh/remesh_local.cpp`'s relax loop:

```cpp
if (delta) delta->note_vertex(surface, verts[i]);
rec->position = targets[i];
if (delta) delta->sync_vertex(surface, verts[i]);
...
surface.refresh_normals(touched);   // rewrites face normals AND every vertex of
                                    // those faces, none of it noted or synced
```

The fix follows the same pattern `DynamicSculptor::write_positions` already uses:

1. Collect the incident faces while computing targets, before any write.
2. Note those faces and all their vertices.
3. Write the positions and refresh the normals.
4. Sync the faces and vertices.

The record grows by the one-ring of the relaxed vertices. The regression test is
the proposal's measurement turned into an assertion: with `relax_after_remesh`
on, the record's `after` equals the live surface immediately after capture, and
undo and redo match `to_mesh` normals exactly.

## Serialization layout

```
u32 'CDGR'  u16 version=1  u16 reserved
u64 lineage  u64 epoch_before  u64 epoch_after
then TopologyDelta::encode() unchanged ('CTDL' v1)
```

That gives `encoded_bytes = 56 + 122*V + 114*H + 42*E + 66*F`. The count test
asserts this formula and that it equals the size query of
`clay_dynamic_delta_serialize`. On the proposal's stroke the inner encoding
matched the formula exactly (1,233,636). The `TopologyDelta` format and
`session::History`'s journal are untouched.

On deserialize, a truncated or hostile buffer is `CLAY_ERROR_INVALID_ARGUMENT`,
before anything is allocated; `TopologyDelta::decode` already refuses these. A
wrapper or inner version above the one this build knows is
`CLAY_ERROR_FORWARD_VERSION`, read from the header before decoding.

## Bindings

- **pyclay** binds C++ directly, so the guard is implemented in C++ where both
  bindings reach it. The Python surface is:
  - a `TopologyDelta` class with `revert(sculptor)`, `apply(sculptor)`, `clear()`,
    `stats` (a dict carrying the struct's fields), `serialize()` and the static
    `deserialize(bytes)`;
  - `DynamicSculptor.stamp(..., record=None)`, mirroring `MeshSculptor.stamp(...,
    deltas=)`.

  A mismatch raises `ValueError` with the SNAPSHOT_MISMATCH reason. In
  `check_binding_parity.py`, `TopologyDelta` maps to `clay_dynamic_delta_`, its
  constructor to `clay_dynamic_delta_create`, and `stamp(record=)` to
  `clay_dynamic_sculptor_stamp_recorded`. Read the gate's output line: `imported`,
  not `parsed`.
- **Swift** consumes `clay.h`. `tests/swift/smoke.swift` gains the capture,
  revert, apply and stats round-trip beside the existing dynamic-surface block.

## Complexity

`DynamicSculptor::replay` is split into `guard`, `restore` and `reindex` so that
each stays within the backend target of 15. The C wrapper for `stamp_recorded`
reuses the descriptor reading that `clay_dynamic_sculptor_stamp` already does, by
extracting it into a helper; it is not copied.

## Follow-up, not in this change

`session::History::undo` of a `DynamicMesh` step resolves a `DynamicSurface`, not
a sculptor, so a C++ caller who keeps a sculptor across it still gets the stale
index from measurement 1. No ABI path reaches it, because `clay_document` has no
dynamic layer. The fix is a sculptor resolver beside `set_dynamic_resolver`. That
is its own change, with its own journal questions.
