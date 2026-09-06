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

- [ ] 1.1 `io::ClaySpaceDoc` gains `multires_layers`, a
      `std::map<scene::LayerId, mesh::MultiresSurface>`, beside `mesh_layers`,
      with the comment stating the layering-table reason as its neighbours do
- [ ] 1.2 Confirm `tools/check_layering.py` still passes and that nothing in
      `clay::scene` can reach the new member — the test is that the rule is
      structural, not that it is followed
- [ ] 1.3 `io::document_memory` counts hierarchies through
      `MultiresSurface::memory()`, and a test asserts the total grows when a
      level is added

## 2. The chunk

- [ ] 2.1 A `'MRES'` chunk: the layer id plus the bytes `encode()` produces. The
      container does NOT version the surface (D3) — assert that the chunk's own
      header carries no surface version field, so a later reader cannot start
      negotiating one
- [ ] 2.2 `kClaySpaceMinor` 18 → 19, reader and writer in the same commit —
      **both halves or neither**. #477 took 18 for a layer's composition while
      this change was in planning; the artifacts said 17 → 18 and are corrected
- [ ] 2.3 Writing at minor 18 REFUSES a document whose hierarchy carries detail
      above its base, and writes the bytes 18 always did for one whose
      hierarchies are bare cages (nothing authored is lost). This REVERSES what
      design.md first said — see D9
- [ ] 2.4 The orphan rule, copied from `mesh_layers` (D4): the entry survives a
      layer removal, the writer emits only for an id that is still a mesh layer,
      the reader drops a chunk naming none
- [ ] 2.5 Regenerate the gallery documents and run `tools/check_gallery.py`

## 3. The C ABI

- [ ] 3.1 Ask whether a layer carries a hierarchy, answered without decoding one,
      and distinguishing "not a mesh layer" from "a mesh layer with none" (D6)
- [ ] 3.2 Take a BORROWED handle for a loaded hierarchy, with
      `clay_multires_destroy` rejecting it exactly as `clay_mask_destroy` rejects
      a borrowed mask (D5), and the header stating the lifetime beside both calls
- [ ] 3.3 A borrowed handle onto a removed hierarchy fails
      `CLAY_ERROR_NOT_FOUND` rather than dangling
- [ ] 3.4 Requesting a handle where there is none is `CLAY_ERROR_NOT_FOUND`, not
      `INVALID_ARGUMENT` — a host walking every layer asks this legitimately
- [ ] 3.5 Attaching to a non-mesh layer is refused; replacing an existing
      hierarchy is explicit rather than silent (D7)
- [ ] 3.6 A query answering whether a layer's cage agrees with its hierarchy's
      base level, computed on demand and NOT stored (0.1). Counts first, then
      positions, so the common divergence is cheap to detect
- [ ] 3.7 A query naming the first layer whose hierarchy blocks an older minor,
      mirroring `scene::layer_blocking_minor` — a refusal a caller cannot NAME
      is one it has to explain by guessing
- [ ] 3.8 Header prose in house style: what these calls do NOT promise, the
      lifetime asymmetry, that reconciling the cage and the base is the HOST's
      (0.1), and that a hierarchy still does not reach the field
- [ ] 3.9 ABI minor bump in the PR that adds the entry points, noted here and in
      the PR body

## 4. pyclay

- [ ] 4.1 The same two questions from Python, following the C ABI's ownership
      rather than inventing a second one
- [ ] 4.2 `tools/check_binding_parity.py` passes with an IMPORTED module — read
      the line it prints, since it cannot fail when it falls back to parsing the
      source against itself
- [ ] 4.3 A pytest round trip: save, reload, assert the level count and that
      dropping the Python reference does not remove the hierarchy

## 5. Tests

- [ ] 5.1 The scenarios in all four spec deltas
- [ ] 5.2 The round trip is bit-identical per level, not merely equal in level
      count — a hierarchy that reloaded as its cage would pass a count assertion
- [ ] 5.3 A document with no hierarchy is byte-identical at minors 18 and 19, so
      the feature costs a document that does not use it nothing
- [ ] 5.4 Prove the round-trip test works by reverting the writer and watching it
      fail, and record that it was proved this way

## 6. Measurement

- [ ] 6.1 Measure what the chunk adds to `clay_document_save` on a real
      hierarchy, and record it. `survive-a-crash` records saves as
      whole-document and synchronous at the `sdf_consolidate` class of cost; this
      adds the largest payload in the document to that path and a host should not
      have to discover the number
- [ ] 6.2 Record the on-disk size against level count, so a host can predict what
      a document will cost before it authors one

## 7. Documentation

- [ ] 7.1 `docs/05-claycore-library.md` — the new entry points and the lifetime rule
- [ ] 7.2 The release note states that writing at minor 18 is REFUSED for a
      document whose hierarchy carries authored detail, and names the query that
      says which layer blocked it
- [ ] 7.3 `openspec/ROADMAP.md`: the host's rank-2 row, with what landed
- [ ] 7.4 `docs/RELEASE.md` at release time, not in this PR
