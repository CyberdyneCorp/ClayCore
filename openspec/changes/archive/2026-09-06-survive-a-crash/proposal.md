# Proposal: survive a crash

## Why

`autosave`, `journal`, `checkpoint` and `crash recover` appear **nowhere** in
this repository. Not in the source, not in the specs, not in
`openspec/ROADMAP.md`. It is not a deferred decision; it is a surface nobody has
drawn.

The library is being pointed at an iPad. An hours-long sculpting session, on an
OS that reclaims memory by killing processes and does not warn twice, against a
history that has **no cap of any kind** (`add-history-budget`, still open, P0).
If the process dies, the session is gone.

**Autosave is the host's policy, and the library has given it no affordable
mechanism.** `clay_document_save` is whole-document and synchronous, and the
device gate says what that costs: `mask_extrude` measures 4403 ms and
`sdf_consolidate` 661 ms as *operations*, and a save of a large sculpt is the
same class of work. A host that autosaves on a timer therefore stalls its UI for
however long the whole document takes, and that cost grows with the sculpt — so
the safer the host tries to be, the worse the stall gets.

## What makes this tractable now, and did not before

Three things landed in the last few changes, and this is the row they were
ordered ahead of:

- **`serialize-without-a-file`** gave the ABI bytes. A journal is bytes appended
  to something the host owns; before that, producing them meant a temporary
  file, which is precisely what a crash leaves behind.
- **`unify-the-undo-history`** gave a history that spans the SDF edit list,
  voxel grids and mesh layers. This is why the order was swapped: a journal
  built on the command vocabulary alone would have recovered an SDF sculpt and
  **silently dropped every voxel and mesh edit** — a recovery that looks like it
  worked and lost half the model, which is worse than no recovery.
- **`scene::serialize(Command)` / `deserialize` already exist**, use the same
  encoding the document format's command chunks use, and are round-trip tested
  (`tests/unit/test_curve.cpp`). They reach **neither binding** — the third
  instance of the same pattern as `report-mesh-quality` and
  `serialize-without-a-file`: the engine has it and the boundary discards it.

## What changes

**Recovery is a snapshot plus the steps since it**, and the split of
responsibility follows the rest of the library: the engine owns the bytes, the
host owns the file.

- **The engine can hand out the journal.** Everything recorded since a step
  index the host names, as one blob it appends wherever it keeps such things —
  a sidecar file, a database row, a package member.
- **The engine can replay a journal** onto a document loaded from the matching
  snapshot, reporting how many steps it applied.
- **The host owns the rest**: where the file lives, when to flush, how often to
  re-snapshot, and what to do with a recovery file on the next launch. Those are
  policy, they differ per platform, and a library that decided them would be
  wrong somewhere.

**A barrier forces a snapshot, and the journal says so.** The history already
records operations no mechanism can reverse. Replay cannot reconstruct past
one, so the journal marks it and the host is told: from here, appending is no
longer enough and the document must be re-snapshotted. This is what stops a
recovery from being *quietly* partial — the failure mode the whole change
exists to avoid.

## What this is NOT

**Not autosave.** The library does not own a timer, a file path, or a policy. It
makes autosave cheap enough for a host to write; it does not write one.

**Not a replacement for saving.** A recovery file is a crash artifact, not a
document. It is paired with a snapshot, it is not portable on its own, and a
host that treats it as a save format will lose data the first time a barrier
lands.

**Not durability.** `fsync`, atomic rename and write ordering are the host's,
because the right answer differs between iOS, a desktop filesystem and a
database, and a library that guessed would be wrong on two of the three.

**Not multi-process.** One writer. Locking a recovery file against a second
process is a host concern and is not modelled here.

**Not resumable long operations.** A crash during a 4.4-second `mask_extrude`
loses that operation, and the journal simply will not contain it. That is
`add-operation-cancellation`'s territory, not this one's.
