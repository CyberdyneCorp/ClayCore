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
- [ ] 1.5 MEASURE the journal's byte rate on a realistic sculpt, so a host can
      size its re-snapshot interval from a number rather than a guess. 16 bytes
      per changed cell is the input; a stroke's worth is the figure

## 2. Decide

- [ ] 2.1 DECIDE and record: does the journal carry the snapshot's identity so a
      replay can refuse a mismatched pair? A hash turns a silently-wrong
      recovery into a refusal
- [ ] 2.2 DECIDE and record: peek with an explicit index, or drain? Peek is
      stateless and lets a host retry a failed write
- [ ] 2.3 DECIDE and record: what an UNDO does to the journal. Either the undo
      is its own entry, or replay reconstructs work the user took back
- [ ] 2.4 DECIDE and record: pyclay's shape, since the parity gate wants an
      answer either way

## 3. Build

- [ ] 3.1 An encoding for `mesh::VertexDeltas` — touched vertices, before/after
      positions, and normals and colours where the record carries them
- [ ] 3.2 An encoding for a voxel step: a run of `{cell, before, after}`
- [ ] 3.3 The journal: versioned, refused rather than partially interpreted when
      a build does not understand it
- [ ] 3.4 `clay_document_journal_since` and `clay_document_replay_journal`,
      returning bytes through `clay_blob` like every other serialized payload
- [ ] 3.5 Barriers in the journal: reported on the way out, and stopping replay
      on the way in
- [ ] 3.6 pyclay, per 2.4

## 4. Prove it

- [ ] 4.1 The scenarios in both spec deltas
- [ ] 4.2 The test this change is for: snapshot, edit across all three
      representations, journal, replay onto a fresh document, and assert it
      evaluates identically and holds the same cells and vertices
- [ ] 4.3 Incremental: journal, edit, journal again from the reported index, and
      replay both in order
- [ ] 4.4 A truncated journal and one from a newer version are REFUSED, leaving
      the document as it was
- [ ] 4.5 A barrier stops replay and is reported, rather than being skipped

## 5. Reach it and say it

- [ ] 5.1 ABI minor bump and `docs/RELEASE.md`
- [ ] 5.2 A section in `docs/05-claycore-library.md` beside the history one,
      saying plainly what a host owns: the file, the flush, the re-snapshot
      interval, and what to do with a leftover recovery file
- [ ] 5.3 A numbered example that kills and recovers a session
- [ ] 5.4 `openspec/ROADMAP.md`
