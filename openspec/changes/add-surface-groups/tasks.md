# Tasks: add-surface-groups

## 0. Establish there is nothing to reuse

- [x] 0.1 Verified against the tree, not assumed. Visibility is per LAYER
      (`clay_document_set_layer_visible`); there is no region form. A layer
      holds exactly ONE mask — `clay_document_add_mask` "replaces any mask the
      layer already had" — so N named regions cannot be emulated with N masks.
      Scene groups group EDIT-LIST NODES, not surface. Nothing in the C ABI
      returns a region id for a surface point

## 1. Decide the representation question first

This is the change. Implementing before deciding it produces a mesh-only
feature wearing a general name.

- [x] 1.1 DECIDED: a world-space id lattice, not a per-item rule. The case the
      task names kills the free answer twice — an armour panel spanning two
      items is not an item, and a face that is part of one sphere is not one
      either. An artist's groups do not respect the edit list.
      Original question:
      A spatial id field on a lattice, like the mask, is one answer and costs
      memory in a representation whose selling point is that it does not. A
      rule mapping a surface point to the item that produced it is the other,
      and it is free but cannot express a group that crosses an item boundary
      or splits one. Decide against a real case — an armour panel that spans
      two items, and a face that is part of one sphere
- [x] 1.2 DECIDED: no voxel storage at all. The same world-space lattice
      answers for a grid, so the second palette channel this worried about —
      doubling a 16.7 M cell guarantee — does not exist.
      Original question:
      palette-indexed channel; measure what it costs on the corpus before
      committing, since the voxel grid's memory profile is a shipped guarantee
- [x] 1.3 DECIDED: they SURVIVE, by construction rather than by transfer. The
      ids were never in the SDF, the voxels or the mesh, so rasterizing,
      meshing or converting cannot lose them. That is the strongest argument
      for the shared lattice.
      Original question:
      bridge. "They are gone" is an acceptable answer and a much better one
      than a transfer that half-works. If they DO survive, it is by attribute
      transfer and that is a dependency, not a detail
- [x] 1.4 DECIDED: per DOCUMENT. A mask is per layer because it gates edits to
      that layer; a group names a region of the MODEL, and "isolate the head"
      when the head spans two layers is the case per-layer storage makes
      impossible.
      Original question:
      simpler and makes "isolate the head" impossible when the head spans two

## 2. The id

- [x] 2.1 Storage per the 1.x decisions. There is only ONE store, which is the
      decision: a world-space `voxel::GroupField` on the same chunked lattice
      shape `MaskField` uses. The SDF answer came first as the task demanded,
      and it invalidated the per-representation design rather than the reverse
- [x] 2.2 `fill(region, id)`, deciding membership at the cell CENTRE so two
      adjacent fills do not overlap by a cell — the rule `MaskField::fill`
      already uses, with a test
- [x] 2.3 `at(world)` — the one query every representation asks, and the
      reason this is not three mechanisms
- [ ] 2.4 Grow, shrink, border — defined on the region, per the spec delta
- [ ] 2.5 Serialisation, with the format minor taken from the ROADMAP's
      assignment rather than picked here

## 3. Partial visibility

- [ ] 3.1 Hide, show, invert, isolate — addressed by group or by mask
- [ ] 3.2 Hidden geometry excluded from meshing and picking, and from display
      evaluation
- [ ] 3.3 Hiding is a command, so it is undoable and it serialises. Note the
      constraint this imposes: on a voxel or mesh layer that is OUTSIDE the
      command vocabulary today (`correct-the-undo-scope`), so either the hidden
      set lives on the document side or this change extends the vocabulary.
      DECIDE which
- [ ] 3.4 Every operation that acts on surface states whether it respects the
      hidden set, and the ones that do not are listed rather than discovered

## 4. Prove it

- [ ] 4.1 The scenarios in the spec delta
- [ ] 4.2 Cross-representation: the same shape in two representations, the same
      group grown once, and the covered regions compared geometrically. This is
      the test that catches a mesh-only implementation wearing a general name
- [ ] 4.3 Hidden survives a save and a reload, on every representation that
      carries groups
- [ ] 4.4 Regression: a document with no groups behaves bit-identically to
      today over the golden corpus

## 5. Reach it and say it

- [ ] 5.1 C ABI
- [ ] 5.2 pyclay
- [ ] 5.3 An example that isolates a region and sculpts it, because the value
      is the workflow and a capability list does not show a workflow
- [ ] 5.4 `docs/sculpt_comparison.md` — the PolyGroups / Face Sets row that
      currently has no entry at all

## 6. What building it settled

- [x] 6.1 Visibility is a property of the ID, not of the cells. Hiding a group
      is one flag rather than a rewrite of every cell carrying it, which is
      also what makes `isolate` cheap — show one, hide the rest
- [x] 6.2 `isolate` leaves kNoGroup VISIBLE. Ungrouped surface is not something
      an artist hid, and isolating a group must not make the rest of the model
      vanish because it was never named
- [x] 6.3 Merging a group away takes its visibility with it, or a hidden id
      nobody carries keeps hiding a group that no longer exists
