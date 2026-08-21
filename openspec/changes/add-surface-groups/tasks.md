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

- [ ] 1.1 DECIDE and record in `design.md`: how an SDF layer carries group ids.
      A spatial id field on a lattice, like the mask, is one answer and costs
      memory in a representation whose selling point is that it does not. A
      rule mapping a surface point to the item that produced it is the other,
      and it is free but cannot express a group that crosses an item boundary
      or splits one. Decide against a real case — an armour panel that spans
      two items, and a face that is part of one sphere
- [ ] 1.2 DECIDE and record: voxel storage. A per-cell id is a second
      palette-indexed channel; measure what it costs on the corpus before
      committing, since the voxel grid's memory profile is a shipped guarantee
- [ ] 1.3 DECIDE and record: what happens to ids across a representation
      bridge. "They are gone" is an acceptable answer and a much better one
      than a transfer that half-works. If they DO survive, it is by attribute
      transfer and that is a dependency, not a detail
- [ ] 1.4 DECIDE and record: are groups per LAYER or per DOCUMENT? Per layer is
      simpler and makes "isolate the head" impossible when the head spans two

## 2. The id

- [ ] 2.1 Storage per the 1.x decisions, one representation at a time, with the
      SDF answer FIRST — it is the one that can invalidate the design
- [ ] 2.2 Assign a region to a group, addressed the way brushes already address
      a region, so a host reuses the vocabulary it has
- [ ] 2.3 Resolve a surface point to its group
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
