# Tasks: instance-a-layer

## 1. Establish what is already there

- [x] 1.1 `scene::Document::instance_layer` copies the `Layer`, takes a fresh
      id and SHARES the `SdfContent` by `shared_ptr`; returns nullptr for a
      source that is not `LayerKind::Sdf`. Reachable only from the unit tests
- [x] 1.2 Evaluation, bounds and culling already understand sharing:
      `scene::node_command_bound` unions over every sharer, `cull_index`
      compiles one tape per layer, `bound-an-edit-across-instances` made both
      ABI dirty-bounds queries union. NOTHING here is rebuilt
- [x] 1.3 `io::layer_memory` / `scene::document_bytes` already charge shared
      content once document-wide and in full per layer

## 2. The three gaps a constructor alone would have shipped

- [x] 2.1 SERIALIZATION: `write_layer` writes the content inline per layer and
      `read_layer` builds a fresh `SdfContent`, so a round trip turns one
      allocation into N and silently unlinks the instances
- [x] 2.2 CONSOLIDATE: `consolidate_layer` edits `layer->sdf` in place, which is
      every sharer's edit list
- [x] 2.3 READ SIDE: nothing in the ABI can say a layer is an instance

## 3. The layer record names its content

- [x] 3.1 `write_layer` takes a content-source layer id, written at minor >= 15
      and followed by the content only when the id is 0
- [x] 3.2 `read_layer` reports it, and leaves `sdf` null for a layer that names
      another
- [x] 3.3 `serialize_document` derives ownership from `shared_ptr` identity in
      STACK ORDER — first holder owns — so a removed source leaves the first
      surviving sharer owning the content
- [x] 3.4 `deserialize_document` resolves the names in a SECOND pass and
      refuses a name that does not resolve
- [x] 3.5 Writing AT minor 14 or below writes every layer's content inline, so
      an older build opens the document with the instances as independent
      copies rather than failing
- [x] 3.6 `kSceneMinor` and `kClaySpaceMinor` to 15 together (the
      `static_assert` in `src/io/clayspace.cpp` binds them), with the note in
      the voice minors 13 and 14 use: what it adds, what an older build sees,
      what it drops if it saves back

## 4. The journal

- [x] 4.1 `AddLayerCmd` gains `content_source`, so an instance creation is a
      reference in the journal too rather than a deep copy
- [x] 4.2 `apply_one(AddLayerCmd)` resolves it when the command carries no
      content, and REFUSES when the id does not resolve — a replay that
      silently deep-copied would be the serialization defect again
- [x] 4.3 The in-memory path is unaffected: the command carries the shared
      pointer and the resolve does not fire

## 5. Consolidate severs

- [x] 5.1 A layer whose content has more than one owner gets a private copy
      before the bake
- [x] 5.2 Expressed as the remove-then-add pair `clay_document_move_layer`
      already uses, inside the group the bake opens — so it is one undo step
      with the bake and undoing it restores the ORIGINAL shared pointer
- [x] 5.3 The bake's node ids are re-derived after the sever; the baked node's
      id is reserved from the PRIVATE content, not the shared one
- [x] 5.4 `clay_layer_consolidation_cost` still only measures and severs nothing

## 6. The C surface

- [x] 6.1 `clay_document_instance_layer`, through `AddLayerCmd` with a reserved
      id so one undo removes the instance
- [x] 6.2 Refusals: an unknown source is `CLAY_ERROR_NOT_FOUND`; a voxel or
      mesh source, a NULL name and an empty name are
      `CLAY_ERROR_INVALID_ARGUMENT` with a message that says which
- [x] 6.3 `clay_layer_info` gains `content_source` and `share_count`, appended;
      `kLayerInfoOriginal` is unchanged so a short `struct_size` still works
- [x] 6.4 `content_source` is derived by the same first-in-stack-order rule the
      writer uses, so it survives a save/load and answers correctly after the
      source layer has been removed

## 7. Documentation

- [x] 7.1 `clay.h`: the call's own prose — what is shared, what is not, and
      every refusal
- [x] 7.2 `clay.h`: `clay_layer_consolidate` and
      `clay_layer_consolidation_cost` say that consolidate severs
- [x] 7.3 `clay.h`: the `clay_layer_info` fields, including what an instance
      reports once its source is gone
- [x] 7.4 `docs/05-claycore-library.md` — a subtool-duplicate section under the
      scene model, and the memory section says the round trip keeps the share
- [x] 7.5 Spec deltas: c-abi, scene-model, file-io

## 8. Tests (`tests/unit/test_c_instance_layers.cpp`, `smoke.c`, ctypes)

- [x] 8.1 Creation, and undo/redo as ONE step
- [x] 8.2 An edit through the instance changes what the source evaluates to,
      and an edit through the source changes what the instance evaluates to
- [x] 8.3 Independent transforms: one edit list, two placements
- [x] 8.4 The instance's name, visibility, protection, mirror and radial start
      at the source's values and diverge
- [x] 8.5 Memory: the content counted ONCE document-wide and in FULL per layer,
      before AND after a save/load round trip — the document total does not
      grow with the instance count
- [x] 8.6 The dirty bounds of an edit cover both placements
- [x] 8.7 Consolidate severs: the other instance keeps its items, and the
      layers stop sharing
- [x] 8.8 The source is removed and the instance still evaluates, still saves
      and still reloads with its content
- [x] 8.9 Every refusal: unknown id, voxel source, mesh source, NULL name,
      empty name
- [x] 8.10 An instance of an instance shares the SAME content
- [x] 8.11 A journal replay of an instance creation shares rather than copies
- [x] 8.12 `clay_layer_info` reports the link, after a reload and after the
      source is gone
- [x] 8.13 A surface drag on a shared layer dirties the OTHER placement: a
      brick over the instance refilled before and after the drag agrees with
      what the document evaluates to
- [x] 8.14 A journal replay of a REORDER of a shared layer keeps the sharing

## 9. Proving the tests

- [x] 9.1 REVERT A: `clay_document_instance_layer` deep-copies the content.
      The shared-edit, dirty-bounds, memory and info cases fail; creation
      passes
- [x] 9.2 REVERT B: write the content inline at minor 15 as the old writer did.
      The round-trip memory and reload-link cases fail
- [x] 9.3 REVERT C: skip the copy-on-write in consolidate. The sever case fails
- [x] 9.4 REVERT D: drop the shared-content guard `tail_append` needed. FOUND
      WHILE IMPLEMENTING, and it is the one defect the constructor exposed
      rather than introduced: the append fast path extends the cached tape for
      the LAYER named, so an append to shared content left every other
      instance stale — an edit through one subtool visibly stopped appearing
      in its duplicates. `command_frontier` already refused the same
      situation, ten lines below; `tail_append` now does too
- [x] 9.5 REVERT E: `write_layer` honours the caller's content source BELOW
      minor 15 too. CAUGHT A REAL DEFECT — the first version did, so a sharer
      written at minor 14 wrote no id AND no content: silent loss on the way
      out and a short record the reader desynchronises on. The older layout
      owns its content whatever the caller says
- [x] 9.6 REVERT F: an unresolvable content source falls back to an empty or
      copied edit list instead of refusing, on both the command path and the
      load path
- [x] 9.7 REVERT G: drop the sharer widening in `clay_layer_move_surface`, so
      the drag invalidates its ball alone. FOUND IN REVIEW: the ball is stated
      in the DRAGGED layer's placement, and the seeds at the other placement
      were advanced to the new revision while still clean and then handed back
      as the whole answer — measured 0.4 world units stale, the whole
      displacement. Every per-command path was already right, because
      `command_influence_bound` unions over the sharers; this is the one call
      that states its reach instead of deriving it
- [x] 9.8 REVERT H: `scene::content_sharer_of` returns 0. FOUND IN REVIEW: a
      reorder is a remove and an add, so the journalled add wrote the shared
      edit list INLINE and a crash recovery came back with the subtools
      unlinked and the edit list multiplied — invisibly, because the shapes
      are right
- [x] 9.9 Each revert COMPILES under `-Werror` before its result is believed.
      Counts, C ABI suite: A 10/16 cases, 30 assertions; B 2/16, 5; C 2/16, 7;
      D 1/16, 1; G 1/16, 1; H 1/16, 4. Command suite: E 1/15, 2; F 2/15, 4.
      Plus the C smoke consumer under A and B

## 10. Gates

- [x] 10.1 `cmake --build build/cpu-only`, `ctest --preset cpu-only`
- [x] 10.2 `check_c_abi.py`, `check_binding_parity.py`, `check_layering.py`,
      `release_check.py`, `openspec validate --strict`
- [x] 10.3 Version to 0.58.0 in CMakeLists.txt, clay.h and pyproject.toml
