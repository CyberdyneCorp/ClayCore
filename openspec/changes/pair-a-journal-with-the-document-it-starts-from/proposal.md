## Why

**A journal started by enabling undo mid-session paired with a stale snapshot
and replayed without the pre-enable edits** (#641).

`clay_document_enable_undo` and pyclay's `Document.enable_undo()` seeded the
crash journal with `document.snapshot_id` — the identity of the bytes the
document was last loaded from or saved to. Nothing clears that id on an edit.
On a document edited after its load and before the enable, `journal_since(0)`
named the loaded snapshot, a replay onto that snapshot was **accepted**, and the
recovered document silently lacked every pre-enable edit. Reproduced on
0f45417d through both bindings:

```text
load snapshot; voxel set (0,0,0); enable_undo; voxel set (1,0,0)
journal_since(0) -> replay onto the snapshot
  before: CLAY_OK, applied 1, recovered cell (0,0,0) == 0   (live: 1)
  after:  CLAY_ERROR_SNAPSHOT_MISMATCH, applied 0
```

The only defence was a documented workaround in `clay.h` and `docs/05`: save
once right after enabling.

## What Changes

**`io::journal_seed_for(const ClaySpaceDoc&)`** — the one answer both bindings'
enable paths now hand `History::note_snapshot`. It encodes the document at this
build's minor *without stamping it* and compares the identity with the snapshot
the document names:

- unchanged -> that snapshot's id, exactly as before;
- edited since -> the identity of the bytes a save would write now. No snapshot
  lacking the edits carries it, so a replay onto one is refused with
  `snapshot_mismatch`; a save made after the enable writes those very bytes and
  pairs;
- never serialized (`snapshot_id == 0`) -> zero, unchanged, and nothing encoded.

**`save_clayspace` is split** into `encode_clayspace` (the bytes) and the stamp
(`save_clayspace`), so asking the question cannot answer it.

**`scene::Document::snapshot_reencoded_id`** — what a loaded stream re-encodes
to, recorded by `load_clayspace` only when the stream is not what this build
writes byte for byte (an older minor, a skipped unknown chunk, a dropped orphan
chunk). Without it an unedited older-minor load would compare unequal and be
refused — the fail-closed direction `survive-a-crash` rejected. Cleared by every
save.

No entry point is added and none changes signature: **no ABI version move.**

## What measuring refuted

- **The issue's framing was that "re-serialize and compare" refuses good
  recoveries whenever a round trip is not byte-canonical.** True of the loaded
  bytes, but the comparison does not have to be against them. Re-encoding a
  non-canonical stream once at load, while the document still IS the snapshot,
  gives the exact answer for older minors at the price of one encode on those
  loads only. A load of this build's own output — whose canonical round trip
  `test_io.cpp` already asserts — pays nothing.
- **An edit census was not needed.** `content_serial` misses voxel edits and
  mesh sculpts, groups and hierarchies have no counter; a dirty flag set by every
  mutating entry point in both bindings would be the largest surface in this
  change and the easiest to let drift. The encode compares the thing itself.

Measured on a 1.58 MB voxel document (80x40 fill-box strips, 8 palette entries),
pyclay, cpu-only Release, median of 15, M-series Mac:

| operation                         | before  | after   |
|-----------------------------------|---------|---------|
| `save` (`to_bytes`)               | 2.27 ms | 2.27 ms |
| `load_bytes`, this build's minor  | 1.41 ms | 1.43 ms |
| `load_bytes`, minor 19            | 1.41 ms | 3.61 ms |
| `enable_undo` on a loaded doc     | ~0      | 2.16 ms |
| `enable_undo`, never serialized   | ~0      | ~0      |

## What building it found

- **A save at an older minor is now refused as a pairing** when undo is enabled
  after it with no edit in between. `save_clayspace` stamped `snapshot_id` for
  any minor while the bindings deliberately note only this build's minor as a
  journal snapshot; enable used to seed whatever was stamped. Whether that minor
  lost anything is not known without loading it back, and refusing is the side
  that cannot recover a document the session never held. Recorded in `clay.h`
  and `docs/05` rather than paid for with a load at every older-minor save.
- **Foreign-writer streams at this build's minor** (chunk order, an empty mask
  chunk, a duplicate chunk) are not detected as non-canonical and an unedited
  load of one would be refused. This build never writes such a stream.

## Impact

- `src/io/clayspace.cpp`, `include/clay/io/clayspace.h`,
  `include/clay/scene/document.h`, `include/clay/session/history.h` (comment).
- `bindings/c/clay_c.cpp`, `bindings/python/pyclay_module.cpp`: the enable path.
- `bindings/c/clay.h`, `docs/05-claycore-library.md`: the documented pairing.
- Spec: `c-abi` — "A session's steps can be journaled and replayed".
