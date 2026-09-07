# Design: survive a crash

## The shape

A recovery is **a snapshot plus the steps since it**. Neither half is new
machinery:

    snapshot = clay_document_save_memory   (serialize-without-a-file, ABI 0.42)
    journal  = the session history's steps, encoded

    clay_document_journal_since(doc, from_step, &blob, &now_at_step)
    clay_document_replay_journal(doc, data, size, &applied, &stopped_at_barrier)

The host writes the snapshot once, appends the journal as it goes, and on the
next launch loads the snapshot and replays. Where the file lives, when it is
flushed, how often to re-snapshot and what to do with a leftover recovery file
are the host's — they differ between iOS, a desktop filesystem and a database,
and a library that decided them would be wrong on two of the three.

## Why incremental, and what it costs

The point is that an autosave costs the edits since the last one rather than the
whole document. The device gate is the argument: `sdf_consolidate` is 661 ms and
`mask_extrude` 4403 ms as *operations*, and a whole-document save of a large
sculpt is that class of work. A host autosaving on a timer would stall its UI
for that long, and the cost grows with the sculpt — the safer it tries to be,
the worse the stall.

A journal of steps is proportional to what changed. A voxel step is 16 bytes per
changed cell (measured — `unify-the-undo-history` task 1.7); an edit-list step
is a command, which is small; a mesh step is its touched vertices.

## What already exists, and the one thing that does not

| step kind | encoding | state |
|---|---|---|
| Scene | `scene::serialize(Command)` / `deserialize`, the same encoding the document format's scene chunk uses, round-trip tested in `tests/unit/test_curve.cpp` | **exists**, reaches neither binding |
| Voxel | a run of `{cell, before, after}`, 16 bytes each, trivially POD | **trivial**, nothing written yet |
| Mesh | `mesh::VertexDeltas` | **absent — this change must add it** |

`VertexDeltas` holds the touched vertex ids and before/after positions, plus
normals and colours when the record carries them, plus the two flags saying
whether it does. That is a straightforward encoding and it is the one genuinely
new serializer here.

**A journal that could encode two of the three kinds is the trap.** It would
recover two thirds of a session and say nothing about the third — the same
shape of failure as journaling commands alone, which is why this change was
ordered after `unify-the-undo-history` rather than before it.

## Barriers are the interesting part

The history records operations no mechanism can reverse — every mask edit
(`voxel::MaskField` is a fourth representation with no history at all),
dropping a resolution level, removing a sculpt layer. Replay cannot reconstruct
past one.

So a barrier **forces a snapshot**, and the journal has to say so. Two rules:

1. Taking the journal tells the host a barrier is in it, so it can re-snapshot
   *before* it needs the recovery rather than discovering the gap during one.
2. Replaying stops at a barrier and reports it, rather than continuing and
   producing a document quietly missing that operation's effect.

Rule 2 is the one that matters. A recovery that silently skips is worse than a
recovery that refuses, because the user cannot see what is missing.

**This makes the mask gap concrete**: today, painting a mask means the journal
can no longer recover the session on its own. That is an argument for bringing
masks into the history, and it belongs to whichever change does that — noted
here rather than smuggled in.

## Versioning

The journal is versioned and a build refuses one it does not understand. It is
NOT the document format and does not share its minor: a recovery file is a crash
artifact paired with one snapshot, not a portable document, and giving it the
document's version would imply a compatibility promise nobody should rely on.

## The snapshot's identity: DECIDED, and it carried code

**YES, and it is automatic.** The journal carries a 64-bit hash of the snapshot
it continues from; a replay onto a document that is not that snapshot is
refused with `CLAY_ERROR_SNAPSHOT_MISMATCH` and nothing applied.

**What was actually at stake.** Without the check, a journal replayed onto the
wrong snapshot does not fail — it SUCCEEDS. Commands name layer ids, and two
sessions of the same shape allocate the same ones, so an `AddNodeCmd` applies
happily to a document that already contains that work; voxel events are written
by absolute cell coordinate onto a grid that never had them. The test measures
the outcome: replaying onto the wrong snapshot left the SDF layer holding two
nodes where the snapshot had one, with nothing on screen to say so. That is the
failure this whole change exists to prevent, arriving through the host's own
bookkeeping rather than through a corrupt file.

**Over the bytes, not a session token.** Two snapshots with the same bytes ARE
the same snapshot: a journal taken against one replays onto the other exactly.
A random per-session id would refuse that pair, so the content hash is
permissive precisely where being permissive is safe, and strict everywhere
else.

**And keyed on the JOURNAL INDEX, which is where the first design was wrong.**
"The last thing this document was serialized to" is the obvious rule and it
breaks on the workflow this change documents. The host is told to *re-snapshot
when the journal grows past the snapshot* — a comparison whose second half a
host may well get by serializing again. Under the obvious rule, that second
serialization repoints a journal the host had ALREADY taken: replayed onto the
snapshot it kept, a correct pair is refused; replayed onto the second image, the
events are ACCEPTED and applied to a document that already contains them. So the
identity is recorded per journal index, and `journal_since(from)` names the
newest snapshot taken at or before `from`. Both directions are pinned by a test.

**An empty segment names no snapshot.** Found by an existing pyclay test going
red: asking below the trimmed floor yields an empty journal, and stamping that
with the session's snapshot turned "you asked below the floor" — a documented
situation with a documented answer — into "wrong snapshot". An empty journal
cannot misapply anything, so refusing it protects nothing.

**One-directional, so an upgrade cannot lose a recovery.** The journal format
goes to version 2 for the identity field, and version 1 is STILL READ, as a
journal that names no snapshot. Refusing those would have made the first launch
after an upgrade discard exactly the recovery file a user is holding after a
crash. The other direction is unchanged and is the safe one: an older build
meeting a version 2 journal refuses it rather than reading the identity as an
event count.

**A typed refusal, and it is the only all-or-nothing one replay has.** A
mismatch is `CLAY_ERROR_SNAPSHOT_MISMATCH`, not `CLAY_ERROR_INVALID_ARGUMENT`,
because the two mean opposite things to a host: unreadable says discard the
file, mismatched says the file is fine and you handed it the wrong snapshot.
The identity is in the header, so nothing has been applied when it is raised —
unlike a truncation, which is caught mid-stream and leaves what it applied.

**What it costs, measured, and the first implementation was too expensive.**
The obvious hash is the byte-at-a-time FNV-1a `session/layer_digest.h` already
uses. On a 1.13 MB snapshot (5 000 items and a 32³ fill) it costs **1.36 ms
against the 1.52 ms save that produced those bytes — 90% on top of every save**,
paid by every host including the ones that never journal. The same idea over
64-bit words runs at 4.7 GB/s against 0.83: **0.24 ms, 15% of the save**. That
is what shipped. A cheaper design was considered and rejected — hashing the head
and tail alone is O(1) and would miss a voxel-only edit in the middle of the
file, which is the incremental case the check exists for.

**What it does NOT catch, stated in the header rather than left to be found.**
Replaying the same journal twice onto the same snapshot: both replays name the
right snapshot and both are accepted. The absolute indices are what guard that,
and they are the host's bookkeeping. It is also not a checksum: a corrupt
snapshot is not detected, and a corrupt journal is caught by being unreadable
rather than by this.

**This task turned out to carry code, not only a decision**: a runtime
`snapshot_id` on `scene::Document` stamped by `io::save_clayspace` and
`io::load_clayspace`, an index-keyed snapshot table on `History`, the version 2
journal header, the new result code, and the checks in both bindings.

## What building 4.1 found: the barrier had lost its last caller

Implementing the scenarios in the two deltas as tests found that **no operation
reachable from a host recorded a barrier at all**. `record_barrier` had exactly
one caller — the mask step — and `masks-in-the-history` correctly took it away
when mask edits became ordinary steps. Nothing replaced it, so the requirement
"a journal says when it stops being enough" was vacuous at the ABI: no journal
could contain a barrier, and the two scenarios about them could not be
provoked through any entry point. This is the same finding as 6.2, one release
later and from the opposite direction: 6.2 wired a caller up, and the change
that made masks recordable unwired the only one there was.

Worse, the operation the documentation names as the example — dropping a
resolution level — recorded NOTHING. A journal replayed across a
`clay_voxel_drop_level` rebuilt a grid that still had the level and every edit
after it, silently, which is exactly the quietly-partial recovery the barrier
mechanism exists to prevent. `clay_voxel_drop_level` records a barrier now, in
both bindings, and the regression test fails without it.

**And a host still had no way to ASK.** Rule 1 of the two this design states —
"taking the journal tells the host a barrier is in it, so it can re-snapshot
BEFORE it needs the recovery" — had no mechanism either: the only report was
`out_stopped_at_barrier` from replay, which arrives during the recovery, the one
moment when being told to take a fresher snapshot is useless. That is
`clay_document_journal_barrier` / `Document.journal_barrier` now.

## Open decisions

1. **DECIDED — see above.** The journal carries a hash of the snapshot it
   continues from, keyed on the journal index, and a mismatch is a typed
   refusal with nothing applied.
2. **DECIDE:** does `clay_document_journal_since` drain or peek? Peek with an
   explicit index is stateless and lets a host retry a failed write; draining is
   fewer parameters and one less thing to get wrong. Leaning peek.
3. **DECIDE:** what happens to the journal when a step is UNDONE. An undo pops a
   step the journal already carries, so either the journal records the undo as
   its own entry, or replay reconstructs a session that includes work the user
   took back. The second is wrong; the first needs an entry kind.
4. **DECIDE:** pyclay's shape. `bytes` in and out matches `Document.to_bytes`,
   but a Python host is less likely to be the one crashing, and the parity gate
   will want an answer either way.
5. **MEASURE, then decide:** the journal's byte rate on a realistic sculpt, so a
   host can size its re-snapshot interval from a number. Task 1.7's 16 bytes per
   changed cell is the input; a stroke's worth is the figure that matters.
