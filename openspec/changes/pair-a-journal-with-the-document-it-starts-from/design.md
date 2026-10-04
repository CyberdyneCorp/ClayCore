## Context

A crash journal names the snapshot it continues from by `snapshot_id`, an FNV
hash of serialized bytes stamped by `io::save_clayspace` and
`io::load_clayspace`. `History::note_snapshot` records (journal index, id)
pairs, and the enable path seeded index 0 with `snapshot_id` unconditionally.
The id describes the bytes, not the document in memory, and edits never cleared
it (#641).

## Goals / Non-Goals

**Goals**
- A journal begun on a document edited since its snapshot never pairs with that
  snapshot.
- An unedited document still pairs, including an older-minor load.
- One implementation, shared by the C and Python bindings.

**Non-Goals**
- An edit census or dirty flag.
- Changing what a never-serialized document's journal names (nothing).
- Making a save at an older minor a pairable journal snapshot.

## Decisions

**Compare the document's encoding, not a count of its edits.** Enabling encodes
the document at `kClaySpaceMinor` and hashes it (`H_now`). If `H_now` equals the
id the document names, the document is that snapshot; otherwise it was edited
since, and the seed is `H_now` itself. `H_now` is a correct id in its own right:
it names the bytes a save would write now, so a save after the enable pairs
without further work, and every snapshot lacking the edits is refused.

Alternatives considered: a per-document dirty flag set by every mutating entry
point in both bindings (large surface, drifts whenever an entry point is
added); seeding an unpairable sentinel when dirty (still needs the flag).

**Encoding is separated from stamping.** `encode_clayspace` produces the bytes;
`save_clayspace` stamps `snapshot_id`. `journal_seed_for` encodes without
stamping, so asking does not turn the document into a snapshot nobody wrote.

**Non-canonical loads record what they re-encode to.** The one way "compare the
encoding" goes wrong is a stream this build would not write byte for byte —
an older minor's header alone differs. `load_clayspace` knows when that can be
the case (minor below this build's, a skipped unknown chunk, an orphan chunk
dropped) and only then re-encodes once, while the document still is the
snapshot, storing the identity in `snapshot_reencoded_id`. `journal_seed_for`
accepts either id as "unchanged" and seeds `snapshot_id`, which is what the
recovery-side load stamps.

`snapshot_reencoded_id` sits beside `snapshot_id` on `scene::Document`, mutable
for the same reason (`save_clayspace` takes a const document), and every save
clears it so a stale value cannot vouch for a later snapshot.

## Risks / Trade-offs

- **Enable is no longer free** on a loaded or saved document: one encode,
  ~one save, once per enable. Enabling is a once-per-session switch.
- **Older-minor loads pay one encode.** This build's own output pays nothing.
- **Fail-closed residue**: an unedited document last *saved* at an older minor,
  and a foreign-writer stream at this minor that is not canonical, are refused
  as pairings. Both are documented; neither is written by this build's normal
  save path.
