# Tasks: bake a document through the pool, not one point at a time

## 1. The fill

- [x] 1.1 `eval::tape_block_fill(tape, ...)` in a new
      `include/clay/eval/bake_volume.h`, returning a
      `field::FieldVolume::BrickBlockFill`. Header-only, beside
      `bake_points.h`, because `eval/` has no translation units and this needs
      to name both `scene::Tape` and `eval::Backend`.
- [x] 1.2 One scratch points buffer for the whole bake rather than one per
      window. `sample_blocks` walks windows in order on one thread, so there is
      nothing to race, and a bake at an interactive cell has thousands of them.
- [x] 1.3 Falls back to the tape's own scalar walk when no CPU backend is
      registered — the same choice `consolidate.cpp`'s `fill_window` makes with
      its optional `BakePointEval`.
- [x] 1.4 The tape is BORROWED and the header says so: it must outlive the
      `sample_blocks` call the fill is handed to. Every call site holds it as a
      local or a stack `shared_ptr` and passes the fill directly into the
      sampling call.

## 2. The verb overloads

- [x] 2.1 `field::relax` takes a `BrickBlockFill` source. Trivial: its document
      form was already exactly sample-then-relax.
- [x] 2.2 `field::flatten` takes one too, applying the blend to the block the
      source filled, IN PLACE, before returning. Not a post-process — see
      `design.md`; baking first would keep the bricks around the source's
      surface and leave the facet in an unsampled band.
- [x] 2.3 The blend is now ONE function, `flatten_at`, shared by both source
      overloads, with the "these settings describe no flatten" guards shared as
      `resolve_plane`. A blend written twice is a blend that drifts, and it
      would make the byte-identity test tautological.

## 3. The call sites

- [x] 3.1 `clay_item_volume_from_document`, `clay_item_volume_relax_from`,
      `clay_item_volume_flatten_from`.
- [x] 3.2 `pyclay`'s `Volume.from_document` and its document-sourced flatten.
      There is no document-sourced relax in `pyclay`.
- [x] 3.3 No ABI change and no signature change at any of them. A host that
      already calls these gets the speed without recompiling against anything
      new.

## 4. Tests

- [x] 4.1 `the pooled tape fill bakes the volume the per-point tape callable
      does`, in `test_consolidate.cpp` beside the bake-identity tests that were
      already there. Four subcases: the plain bake, relax, flatten, and
      flatten's no-plane guard branch, which the batched overload reaches
      through a different function.
- [x] 4.2 The fixture is a POLISHED sphere, not a plain one. A steep field
      keeps bricks holding values far past the band, so a fill that disagreed
      about a sample near a brick's edge changes which bricks survive rather
      than only what they hold — and `serialize()` compares both.
- [x] 4.3 `==` on `serialize()`, not a tolerance. `eval_points` writes to
      disjoint slices and each point goes through the same scalar reference
      arithmetic, so one ULP of difference is a defect.
- [x] 4.4 Full unit suite: 1412 cases, no failures. `clay_c_smoke` OK.
      `examples/20_relax.py`, `21_flatten.py` and `38_consolidation.py` all
      exit 0, which is the pyclay side driven end to end.

## 4b. Module layering

- [x] 4b.1 `bake_volume.h` is the first `eval -> field` include in the tree.
      The edge is added to `ALLOWED` in `tools/check_layering.py` with the
      reason, beside the `voxel -> field` and `brush -> field` exceptions it
      matches: no cycle, and nothing new in the transitive graph, since `eval`
      already depends on `scene` and `scene` on `field`.
- [x] 4b.2 `python3 tools/check_layering.py` OK, and the rest of that CI job —
      kernel dialect, the kernels artifact, the license manifest, binding
      parity, SwiftPM linkage — verified locally behind it.

## 5. The gate

- [x] 5.1 `BM_VolumeBakeDoc` and `BM_VolumeBakeSerialDoc`, the second kept as
      the reference the way `BM_ConsolidateSerialGrownDoc` is.
- [x] 5.2 The pair added to `FASTER_THAN` in `tools/check_bench.py`. This is
      the point of the change as much as the speed is: `bake_layer` was gated
      and these three were not, which is how they kept the serial walk long
      after `bake_layer` stopped using it.
- [x] 5.3 Verified: 23.4 ms against 383.6 ms, gate reports the pair.

## 6. Not in this change

- [ ] 6.1 `move_topological`'s document form (#275). It samples at a DISPLACED
      point, so a batched fill has to build its own query positions rather than
      post-process the lattice, and `solve()` is a second consumer with its own
      access pattern.
- [ ] 6.2 The duplicate Lipschitz measurement: `flatten`, `move_topological`
      and `mask_extrude` each measure a volume `sample_blocks` has already
      measured. Free to fix, real, and not this change.
- [ ] 6.3 Whole-band relax traversal (#272) — the last O(field-size) term on
      this path, and the one that changes a scaling law rather than a constant.
