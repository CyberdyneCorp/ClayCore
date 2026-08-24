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

## Open decisions

1. **DECIDE:** does the journal carry the snapshot's identity, so a replay can
   refuse a journal paired with the wrong snapshot? A hash of the snapshot bytes
   is cheap and turns a silently-wrong recovery into a refusal. Leaning yes.
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
