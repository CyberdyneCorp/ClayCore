# Tasks: survive-a-crash

## 1. Measure before designing

- [x] 1.1 Confirm the surface is absent rather than deferred: `autosave`,
      `journal`, `checkpoint` and `crash recover` appear nowhere in the source,
      the specs or `openspec/ROADMAP.md`
- [x] 1.2 Confirm `scene::serialize(Command)` / `deserialize` exist, use the
      encoding the document format's scene chunk uses, are round-trip tested in
      `tests/unit/test_curve.cpp`, and reach NEITHER binding — the third
      instance of the pattern `report-mesh-quality` and
      `serialize-without-a-file` also closed
- [x] 1.3 Confirm `mesh::VertexDeltas` has NO encoding. It is the one genuinely
      new serializer this change needs
- [x] 1.4 Confirm why this had to follow `unify-the-undo-history`: a journal of
      commands alone would recover an SDF sculpt and silently drop every voxel
      and mesh edit
- [x] 1.5 MEASURED, and it corrected the proposal's headline. Three ordinary
      edits journal **507 bytes against a 3595-byte re-save — 7.1x cheaper**.
      But "always cheaper" is FALSE: a journal entry is raw (14 bytes per
      changed cell) while the document stores voxels palette- and
      RLE-compressed, so one big `fill_box` journals **7189 bytes against a
      590-byte document**. The rule a host needs is *re-snapshot when the
      journal grows past the snapshot*, which is a comparison it already has
      both sides of. Both figures are asserted in `examples/60`

## 2. Decide

- [ ] 2.1 DECIDE and record: does the journal carry the snapshot's identity so a
      replay can refuse a mismatched pair? A hash turns a silently-wrong
      recovery into a refusal
- [x] 2.2 DECIDED: peek, with absolute indices and an explicit `trim`. A
      failed write is retried by asking again; indices do NOT shift on trim, so
      a host that asks below the floor is told it is gone rather than handed
      the wrong events
- [x] 2.3 DECIDED: an undo is its own EVENT. The journal is an append-only
      log, not a view of the step list — a host persists a step, the user undoes
      it, and a journal read off the step list would no longer contain it while
      the host's file still does. Pinned by a test
- [x] 2.4 DECIDED: bytes in and out, matching `Document.to_bytes`.
      `journal_since` returns `(bytes, now_at)` and `replay_journal` a dict, so
      the two out-parameters each side needs stay named rather than positional

## 3. Build

- [x] 3.1 An encoding for `mesh::VertexDeltas` — touched vertices, before/after
      positions, and normals and colours where the record carries them
- [x] 3.2 An encoding for a voxel step: a run of `{cell, before, after}`
- [x] 3.3 The journal: versioned, refused rather than partially interpreted when
      a build does not understand it
- [x] 3.4 `clay_document_journal_since`, `_range`, `_trim` and
      `clay_document_replay_journal`, returning bytes through `clay_blob`
- [x] 3.5 Barriers in the journal: reported on the way out, and stopping replay
      on the way in
- [x] 3.6 pyclay: journal_since / journal_range / journal_trim / replay_journal

## 4. Prove it

- [ ] 4.1 The scenarios in both spec deltas
- [x] 4.2 The test this change is for: snapshot, edit across all three
      representations, journal, replay onto a fresh document, and assert it
      evaluates identically and holds the same cells and vertices
- [x] 4.3 Incremental: journal, edit, journal again from the reported index, and
      replay both in order
- [x] 4.4 A truncated journal and one from a newer version are REFUSED, leaving
      the document as it was
- [x] 4.5 A barrier stops replay and is reported, rather than being skipped

## 5. Reach it and say it

- [x] 5.1 ABI minor bump and `docs/RELEASE.md`
- [x] 5.2 A section in `docs/05-claycore-library.md` beside the history one,
      saying plainly what a host owns: the file, the flush, the re-snapshot
      interval, and what to do with a leftover recovery file
- [x] 5.3 A numbered example that kills and recovers a session
- [ ] 5.4 `openspec/ROADMAP.md`

## 6. What building it changed

- [x] 6.1 The journal is at COMMAND grain, not step grain, and the first draft
      was not. A `Step::Kind::Scene` names an entry on the wrapped `UndoStack`
      and does not carry the command, and one entry can be a coalesced stroke
      or a whole group — so a step-grain journal could encode voxel and mesh
      steps and had *nothing to write* for an SDF edit. That is the
      two-of-three trap this change was ordered after `unify-the-undo-history`
      to avoid, met from a different direction. Recording commands and replaying
      them through `perform()` also makes coalescing and grouping reproduce
      themselves instead of having to be re-derived

- [x] 6.2 The barrier claim was ASPIRATIONAL until this slice. `record_barrier`
      existed and **nothing ever called it**, so "a mask edit is a barrier" was
      documented, asserted in an example, and false. Every mask mutator in both
      bindings records one now — 11 entry points in C, 11 in pyclay — and
      `PyMaskField` gained the history reference `PyVoxelGrid` already had,
      which is the same binding asymmetry twice
