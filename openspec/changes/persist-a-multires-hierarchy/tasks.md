# Tasks: persist a multires hierarchy

**SEQUENCING — satisfied.** This waited for PR #477
(`fold-the-layers-with-an-operator`), which landed 2026-09-06 and took
`kClaySpaceMinor` 18 and ABI 0.87.0. Both numbers moved under this change while
it was in planning, and every artifact here is corrected to the tree rather than
to what it said when written.

## 0. The decision the design could not make from reading

- [x] 0.1 DECIDED 2026-09-06: **store both verbatim, reconcile neither, and let
      a host ASK whether they agree.** What the tree does today settles it —
      `clay_multires_from_mesh` takes a `clay_mesh*`, not a layer, so a hierarchy
      is built from a mesh VALUE and holds its own copy of the cage with no link
      back. The two can already diverge, and today the divergence is invisible
      because the hierarchy is not in the document at all. Persisting both
      therefore does not create the hazard; it makes it observable.

      Rejected, with reasons: **refusing the direct edit** changes what mesh-layer
      editing does for every existing host, to defend an invariant that has never
      held; **re-projecting on save** makes a save mutate authored content, which
      no other payload here does; **declaring the hierarchy authoritative** turns
      the layer's triangles into a cache that meshing, pick and export all read,
      and nothing keeps it fresh.

      So the round trip is an IDENTITY — the same two objects the host has today,
      minus the side-car — matching `mesh_layers`' own "stored exactly as the
      importer returned it, so the round trip is an identity". Reconciliation
      stays the host's, exactly as it is now, and the header says so.

      **The one thing added is a query**: does this layer's cage agree with its
      hierarchy's base level. Computed on demand from two things already in
      memory, in the shape `snapshot_identity` uses for the crash journal — but
      NOT stored. That function documents itself as stable "neither across builds
      that change the document encoding nor across byte orders", so a hash
      written into the file would report a false divergence the first time an
      encoding moved. Comparing at load costs a walk of the cage and cannot go
      stale (see 3.8)

## 1. Storage

- [x] 1.1 `io::ClaySpaceDoc` gains `multires_layers`, a
      `std::map<scene::LayerId, mesh::MultiresSurface>`, beside `mesh_layers`,
      with the comment stating the layering-table reason as its neighbours do
- [x] 1.2 Confirm `tools/check_layering.py` still passes and that nothing in
      `clay::scene` can reach the new member — the test is that the rule is
      structural, not that it is followed
- [x] 1.3 `io::document_memory` counts hierarchies through
      `MultiresSurface::memory()`, and a test asserts the total grows when a
      level is added

## 2. The chunk

- [x] 2.1 A `'MRES'` chunk: the layer id plus the bytes `encode()` produces. The
      container does NOT version the surface (D3) — assert that the chunk's own
      header carries no surface version field, so a later reader cannot start
      negotiating one
- [x] 2.2 `kClaySpaceMinor` 18 → 19, reader and writer in the same commit —
      **both halves or neither**. #477 took 18 for a layer's composition while
      this change was in planning; the artifacts said 17 → 18 and are corrected
- [x] 2.3 REFUTED, and there was nothing to build. Both this task and D9 assumed
      the container could be asked to write at an older minor, by analogy with
      `scene::serialize_document(doc, minor)`. It cannot: `save_clayspace` takes
      no minor and always writes `kClaySpaceMinor`, and the older-minor
      discipline in this format lives in the SCENE PAYLOAD. So there is no
      downgrade to refuse and no `multires_blocking_minor` worth adding — it
      would answer about a write nobody can request.

      What IS true, and is worse, is recorded at the minor instead: a new chunk
      is minor 13's mild kind, skipped by an older reader — but an older build
      that opens a document and SAVES IT BACK drops the hierarchy. Minor 13
      could call that the safe direction for groups ("geometry reappearing is
      recoverable and obvious"); for a sculpt it is not. Nothing here can stop
      an older build, so the header says so and `multires_carries_detail` is
      what a host asks to know whether a document has anything to lose
- [x] 2.4 The orphan rule, copied from `mesh_layers` (D4): the entry survives a
      layer removal, the writer emits only for an id that is still a mesh layer,
      the reader drops a chunk naming none
- [x] 2.5 Regenerate the gallery documents and run `tools/check_gallery.py`

## 3. The C ABI

- [x] 3.1 Ask whether a layer carries a hierarchy, answered without decoding one,
      and distinguishing "not a mesh layer" from "a mesh layer with none" (D6)
- [x] 3.2 Take a BORROWED handle for a loaded hierarchy, with
      `clay_multires_destroy` rejecting it exactly as `clay_mask_destroy` rejects
      a borrowed mask (D5), and the header stating the lifetime beside both calls
- [x] 3.3 A borrowed handle onto a removed hierarchy fails
      `CLAY_ERROR_NOT_FOUND` rather than dangling
- [x] 3.4 Requesting a handle where there is none is `CLAY_ERROR_NOT_FOUND`, not
      `INVALID_ARGUMENT` — a host walking every layer asks this legitimately
- [x] 3.5 Attaching to a non-mesh layer is refused; replacing an existing
      hierarchy is explicit rather than silent (D7)
- [x] 3.6 A query answering whether a layer's cage agrees with its hierarchy's
      base level, computed on demand and NOT stored (0.1). Counts first, then
      positions, so the common divergence is cheap to detect
- [x] 3.7 NOT BUILT, see 2.3: there is no minor-taking save for it to answer
      about, so it would be an entry point with no caller. The header records
      the absence and the reason, so the next reader does not re-derive it
- [x] 3.8 Header prose in house style: what these calls do NOT promise, the
      lifetime asymmetry, that reconciling the cage and the base is the HOST's
      (0.1), and that a hierarchy still does not reach the field
- [x] 3.9 ABI minor bump in the PR that adds the entry points, noted here and in
      the PR body

## 4. pyclay

- [x] 4.1 The same two questions from Python, following the C ABI's ownership
      rather than inventing a second one
- [x] 4.2 `tools/check_binding_parity.py` passes with an IMPORTED module — read
      the line it prints, since it cannot fail when it falls back to parsing the
      source against itself
- [x] 4.3 A pytest round trip: save, reload, assert the level count and that
      dropping the Python reference does not remove the hierarchy

## 5. Tests

- [x] 5.1 The scenarios in all four spec deltas
- [x] 5.2 The round trip is bit-identical per level, not merely equal in level
      count — a hierarchy that reloaded as its cage would pass a count assertion
- [x] 5.3 A document with no hierarchy is byte-identical at minors 18 and 19, so
      the feature costs a document that does not use it nothing
- [x] 5.4 Prove the round-trip test works by reverting the writer and watching it
      fail, and record that it was proved this way

## 6. Measurement

- [x] 6.1 Measure what the chunk adds to `clay_document_save` on a real
      hierarchy, and record it. `survive-a-crash` records saves as
      whole-document and synchronous at the `sdf_consolidate` class of cost; this
      adds the largest payload in the document to that path and a host should not
      have to discover the number
- [x] 6.2 Record the on-disk size against level count, so a host can predict what
      a document will cost before it authors one

## 7. Documentation

- [x] 7.1 `docs/05-claycore-library.md` — the new entry points and the lifetime rule
- [ ] 7.2 `docs/RELEASE.md` at release time states what an OLDER build does to a
      document carrying a hierarchy: it opens it, skips the chunk, and drops the
      hierarchy if it saves back. Not a refusal — see 2.3 — a one-directional
      loss this format has not had in the unsafe direction before
- [x] 7.3 `openspec/ROADMAP.md`: the host's rank-2 row, with what landed
- [ ] 7.4 `docs/RELEASE.md` at release time, not in this PR


## What building it found

Five things the plan had wrong, each recorded where it is load-bearing rather
than only here.

1. **There is no container downgrade to refuse** (2.3, D9). The whole refusal
   design was an analogy with the scene minor that does not hold.
2. **`kClaySpaceMinor` and `scene::kSceneMinor` are tied by a `static_assert`**
   and must move together, so minor 19 moves the scene layout version with no
   new scene field — minor 10's case, for minor 10's reason.
3. **`MultiresSurface` is move-only**, so attaching is `clay_layer_take_multires`
   rather than a setter taking a copy. A copying form would round trip through
   encode/decode: on the 4-level fixture below that is 1.2 MB and a full decode
   on a call a host reads as bookkeeping.
4. **`clay_multires_destroy` returns `void`** where `clay_mask_destroy` returns
   `clay_result`, so the borrowed-handle REFUSAL the mask precedent documents
   cannot be copied. Freeing a borrowed handle is safe rather than tolerated —
   it owns nothing — so it frees the wrapper and the header states the
   difference.
5. **`MemoryReport` said a document cannot walk a surface.** True of every
   surface a host built and no longer true of one the document carries, so
   `document_memory` walks these and the doc comment is corrected.

**The measurement, and the part a host cannot guess.** A 289-vertex cage
sculpted at its finest level:

| levels | vertices at top | document bytes | save |
|---:|---:|---:|---:|
| none | — | 9,750 | 0.012 ms |
| 1 | 1,601 | 44,090 | 0.053 ms |
| 2 | 6,273 | 105,582 | 0.132 ms |
| 3 | 24,833 | 326,870 | 0.402 ms |
| 4 | 98,817 | 1,211,926 | 1.546 ms |

**Cost follows AUTHORED DETAIL, not level count.** The same four-level hierarchy
with nothing sculpted into it adds **128 bytes** and no measurable time, because
the detail field is sparse. A host cannot price a document from its level count,
and a deep hierarchy nobody has touched is nearly free to carry.
