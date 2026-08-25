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
- [x] 2.4 Grow, shrink, border — on the lattice, so "grow on a mesh" and "grow
      on a voxel grid" are the same operation rather than two that agree by
      inspection. 6-neighbourhood, not 26: a diagonal step grows by sqrt(3)
      along the diagonal and 1 along an axis, so "grow by two" would mean two
      different distances depending on which way the surface ran.
      GROWTH CLAIMS ONLY UNGROUPED CELLS. Growing into a neighbour would
      silently destroy a region an artist named, and nobody expects "grow" to
      delete. Tested against a face-adjacent neighbour, which is the case that
      would catch it.
      The frontier is collected BEFORE anything is written, or a cell claimed
      this step seeds the next one and grow(1) walks the whole lattice — pinned
      by a test asserting one cell becomes exactly seven.
      HONEST LIMIT, recorded in the header, the ABI, the docs and the example:
      growth is VOLUMETRIC, not geodesic. ZBrush grows along the surface; this
      dilates in 3D, so a fold closer than `steps` cells is crossed rather than
      followed. The lattice knows regions, not surfaces
- [x] 2.5 A `'GRUP'` chunk at `.clayspace` minor **13**, ONE per document
      rather than one per layer, because the lattice is per document. RLE over
      16-bit ids, and the HIDDEN SET rides in the same blob — a document that
      reloaded its groups and forgot which were hidden would show an artist
      geometry they had put away, and "hiding is not deleting" has to survive a
      save to mean anything.
      A NEW CHUNK, so this is the mild kind of format change: length-prefixed,
      and a build that predates 13 skips it and opens the document with no
      groups. The one-directional loss is louder here than for minor 5 and is
      recorded in RELEASE.md — such a build saving back drops every group and
      every hidden flag, so a hidden region reappears. That is the safe
      direction: geometry reappearing is obvious, geometry silently staying
      hidden is not

## 3. Partial visibility

- [x] 3.1 Hide, show, invert, isolate. "Or by mask" is `fill_from_mask` rather
      than a second visibility system: a host paints a mask however it likes —
      a brush, a cavity measure, an extrude — and NAMES the result, so there is
      one hidden set with one set of semantics and two ways in. Two mechanisms
      for one intent can disagree; one cannot
- [x] 3.2 The half that was missing, and the reason this change was reported
      landed while being unusable: `GroupField` could SAY a point was hidden and
      NOTHING ASKED.
      MESHING: `voxel::drop_hidden` drops faces whose CENTROID is hidden — the
      centroid rather than any vertex, because a face spanning the boundary has
      to go one way and only the centroid makes the kept region agree with the
      group's extent instead of eroding or dilating it by one face.
      A TRIANGLE FILTER AND NOT A CHANGE TO THE FIELD, which is the design
      decision here. Making hidden regions evaluate as empty is the obvious
      implementation and is wrong twice: it changes what the DOCUMENT MEANS
      (the invariant check_layering.py protects by withholding clay/voxel from
      clay::scene), and it carves a hard boundary into the surface so the
      mesher closes the hole with new geometry — showing the group again would
      not restore the original triangles and "hiding is not deleting" would be
      false. Filtering the produced mesh is exactly reversible, and a test
      asserts the triangle count returns to the byte.
      QUAD-AWARE, and not a refinement: mesh_data.h makes it a RULE that
      rewriting `indices` clears `quads`, so a triangle-wise filter would hand
      back a quad export carrying no quads — defeating the one thing that
      export is for. Quad q owns corners quads[4q..] and triangles
      indices[6q..], so dropping both together keeps the arrays in lockstep by
      construction.
      PICKING: a hidden hit is SKIPPED and the march continues, not turned into
      a miss — hiding the front of a head is how an artist reaches the inside,
      so stopping there would defeat the feature. Wired on all four paths
      (clay_raycast, _many, _attributed, and pyclay's), which took three
      attempts — see 3.5.
      DISPLAY EVALUATION: not gated, deliberately, and listed under 3.4
- [x] 3.3 DECIDED: the hidden set lives on the DOCUMENT side, and the history
      records it as a new step kind rather than the command vocabulary being
      extended. The lattice already sits beside the document where masks and
      voxel grids do, and `unify-the-undo-history` had since built exactly the
      mechanism for a representation the command stack cannot see.
      `Step::Kind::SurfaceGroup`, spelled apart from `JournalEvent::Kind::
      GroupBegin/GroupEnd` — which mean COMMAND grouping — because this enum now
      has to hold two unrelated senses of "group" at once.
      A WHOLE SNAPSHOT where every other kind stores a diff, and deliberately: a
      voxel step diffs because a stroke is hundreds a second against a grid of
      megabytes, and group edits are the opposite on both counts — a handful a
      session against an RLE'd field of kilobytes. It also gets something a cell
      diff would not, since one edit can change ids AND visibility (isolate does
      both) and a snapshot reverses both without two payloads kept in step.
      A THIRD bracket mechanism, for the reason the mask needed a second:
      GroupField has no single write choke point either, and two of its
      mutators touch only the hidden set and no cell at all.
      An edit that changed nothing is dropped, so isolating the group already
      isolated does not put a do-nothing undo in the menu
- [x] 3.4 Listed in `clay.h`, in `docs/05-claycore-library.md` and in the
      pyclay docstrings, because a brush that silently reaches hidden surface is
      worse than one that refuses.
      RESPECTS: clay_document_mesh, _mesh_quads, clay_raycast, _many,
      _attributed, and their pyclay equivalents.
      DOES NOT, each with the reason stated rather than left implicit:
      clay_document_eval and every field query (the field is what the document
      MEANS and a group must not change it); every brush and voxel verb (a brush
      is bounded by its footprint and by a MASK, which is the existing mechanism
      for "do not edit here" — gating on visibility too would be two mechanisms
      for one intent that can disagree; isolate to SEE, mask to PROTECT);
      clay_document_save (the whole document is written, or hiding becomes a
      form of deleting); the mesh exporters (they take a mesh you already have,
      and the filter ran when you meshed)

## 4. Prove it

- [x] 4.1 The scenarios in the spec delta, in C++, in C and in pyclay
- [x] 4.2 THE FIRST VERSION WAS VACUOUS and it is worth recording. It asserted
      `g.at(p) == g.get(g.cell_at(p))`, which is the inline DEFINITION of at()
      — an identity that holds however wrong the field is. It compared the
      mechanism to itself and ran 1700 assertions doing it.
      Replaced with a real one: an actual SDF sphere meshed to triangles AND
      rasterized to cells, then group membership compared at corresponding
      points of the two. Those are built by different code from different
      lattices, so agreeing is a claim rather than a tautology
- [x] 4.3 Ids and the hidden set both, at field level, at document level and
      through pyclay. Also that re-serialising a round-tripped field is
      BYTE-IDENTICAL, or a document's bytes would depend on how many times it
      had been opened
- [x] 4.4 A document with no groups writes no chunk and reads back with none,
      and an EMPTY lattice is the same as none — asserted by comparing saved
      sizes. `drop_hidden` returns 0 and does not touch the mesh when nothing is
      hidden, which is what makes it safe to call on every meshing path. The
      full corpus is green: 1383 unit tests, 469 pytest

## 5. Reach it and say it

- [x] 5.1 C ABI: `clay_groups` and 19 entry points. The handle is ALWAYS a
      borrow — there is no standalone form, unlike a mask, because a group names
      a region of a MODEL and a standalone lattice would name a region of
      nothing
- [x] 5.2 pyclay: `Document.groups()` / `.has_groups` and a `GroupField` class
- [x] 5.3 `examples/63_surface_groups.py`, which names a band SPANNING TWO
      ITEMS — the case that kills the "map a surface point to the item that made
      it" shortcut — isolates a region, sculpts on what is left, and shows
      hide-then-show restoring the triangle count exactly.
      THE RENDER CAUGHT A BUG NO TEST DID: the first run produced BYTE-IDENTICAL
      images for "whole" and "isolated", because _render.py draws through
      pyclay's raycast_many and only the C ABI's raycast paths had been wired.
      Two of the four picking paths were unwired and every test passed
- [x] 5.4 The PolyGroups / Face Sets row, plus a section in
      `docs/05-claycore-library.md` carrying the what-respects-hidden table, and
      the Slice/Knife row amended — naming the two halves is possible now,
      splitting the solid still is not

## 6. What building it settled

- [x] 6.1 Visibility is a property of the ID, not of the cells. Hiding a group
      is one flag rather than a rewrite of every cell carrying it, which is
      also what makes `isolate` cheap — show one, hide the rest
- [x] 6.2 `isolate` leaves kNoGroup VISIBLE. Ungrouped surface is not something
      an artist hid, and isolating a group must not make the rest of the model
      vanish because it was never named
- [x] 6.3 Merging a group away takes its visibility with it, or a hidden id
      nobody carries keeps hiding a group that no longer exists

- [x] 6.4 A NAMING TRAP worth recording: `JournalEvent::Kind` already spent
      `GroupBegin`/`GroupEnd` on COMMAND grouping, an unrelated idea — one
      bundles edits into a step, the other names a region of the model. The new
      kind is `SurfaceGroup` rather than `Group` so the enum reads unambiguously
      instead of relying on context
- [x] 6.5 SKIPPING A HIDDEN HIT TOOK THREE ATTEMPTS, and the two wrong ones are
      instructive. Advancing past the hit by an epsilon leaves the ray INSIDE
      the shape it just hit, where the field is negative and a sphere-march
      cannot step at all — the re-cast simply misses. Then walking forward until
      the field goes positive again does not work either, for a subtler reason:
      the point where it goes positive IS the far wall, the very surface being
      looked for, so that walk overshoots the answer by construction. What is
      actually wanted is the next SIGN CHANGE whose point is visible, so
      `pick::next_visible_crossing` scans and bisects for one
- [x] 6.6 The history's byte accounting missed `group_before`/`group_after` on
      first write — two whole serialised fields, which for a SurfaceGroup step
      is the term that matters rather than a rounding error. The same omission
      `roll-up-document-memory` had just found six of in `node_bytes`, caught
      here only because that work was fresh
