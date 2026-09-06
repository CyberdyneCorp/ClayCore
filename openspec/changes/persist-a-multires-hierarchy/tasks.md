# Tasks: persist a multires hierarchy

**SEQUENCING.** Implementation waits for PR #477
(`fold-the-layers-with-an-operator`) to land. It touches
`include/clay/io/clayspace.h` and regenerates nine gallery `.clayspace`
documents, which is exactly this change's blast radius. Two branches did the same
archive sweep on the same day already; this one is avoidable by waiting.

## 0. The decision the design could not make from reading

- [ ] 0.1 DECIDE and record: the base cage exists twice — as the mesh layer's
      `mesh::Mesh` and as the hierarchy's base level — and nothing forces them
      equal. Determine what the tree does TODAY when a mesh layer carrying a
      hierarchy is edited directly (refuse, diverge silently, or re-project), by
      writing the case rather than by reading. Then choose: refuse the direct
      edit, re-project on save, or declare the hierarchy authoritative and the
      layer's triangles a cache. Record which, and what the other two would have
      cost. **This gates 2.x** — a round trip cannot be specified until it is
      known which of the two copies is the truth

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
- [ ] 2.2 `kClaySpaceMinor` 17 → 18, reader and writer in the same commit —
      **both halves or neither**
- [ ] 2.3 Writing at minor 17 omits the chunk and reproduces exactly the bytes 17
      produced before hierarchies existed
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
- [ ] 3.6 Header prose in house style: what these calls do NOT promise, the
      lifetime asymmetry, and that a hierarchy still does not reach the field
- [ ] 3.7 ABI minor bump in the PR that adds the entry points, noted here and in
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
- [ ] 5.3 A document with no hierarchy is byte-identical at minors 17 and 18, so
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
- [ ] 7.2 The release note states exactly what writing at minor 17 loses: the
      levels, the detail field and the sculpt-layer stack. The format rule
      requires the older minor to be writable; it does not by itself make the
      loss legible
- [ ] 7.3 `openspec/ROADMAP.md`: the host's rank-2 row, with what landed
- [ ] 7.4 `docs/RELEASE.md` at release time, not in this PR
