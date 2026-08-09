# Tasks: cache-the-compiled-tape

## 1. The cache

- [x] 1.1 `clay_document` holds the compiled tape and the picking tape in
      separate slots, keyed on an atomic revision
- [x] 1.2 Readers get a `shared_ptr` snapshot built under a lock, not a
      reference into a slot another thread may rebuild
- [x] 1.3 Every reader routed through it: `clay_eval_points`,
      `clay_eval_gradients`, `clay_safe_step_scale`, `clay_document_mesh`,
      `clay_raycast`, `clay_raycast_many`, `clay_raycast_attributed`, and the
      volume/voxel readers

## 2. Invalidation

- [x] 2.1 `apply_edit` bumps — the funnel for the entire command vocabulary,
      so all 17 commands invalidate in one place
- [x] 2.2 Undo and redo bump: they replay commands straight onto the document
      rather than through `apply_edit`
- [x] 2.3 Adding a voxel layer bumps, since it appends to `document.layers`
      even though `compile_document` skips non-SDF layers. Bumping where it may
      not be needed costs one recompile; not bumping where it is needed is
      silent corruption, so the rule is to bump.

## 3. The guarantee

- [x] 3.1 `tests/unit/test_c_tape_cache.cpp` walks the mutating surface — add
      item (both paths), remove node, set transform / prim / colour / op+blend,
      add deformer, append and trim stroke, hide layer, transform layer, mirror
      layer, add layer, remove layer, apply brush stroke, undo, redo — and
      requires the field's ANSWER to change. A test that only checked the call
      returned `CLAY_OK` would pass against a permanently stale cache.
- [x] 3.2 Ghosting is checked against BOTH tapes: picking changes, evaluation
      does not, and un-ghosting restores the original picking answer exactly.
- [x] 3.3 Concurrency: four threads reading one document, and separately 8
      threads x 20 000 reads under ThreadSanitizer, with zero mismatches.
- [x] 3.4 Mutation-proven. Disabling `touch()` fails 19 assertions across every
      path. Removing only the undo/redo site fails the "undo and redo" subcase.

## Measurements

At 2 680 nodes (240 short strokes), desktop Release:

| pattern | before | after |
|---|---|---|
| repeated reads, `clay_eval_points` 1 pt | 0.417 ms | 0.074 ms (−82%) |
| repeated reads, `clay_raycast` | 0.954 ms | 0.622 ms (−35%) |
| ten reads per edit | 0.961 ms | 0.597 ms (−38%) |
| one read per edit | 0.944 ms | unchanged |

## Found while building

- [x] 4.1 The first version of the mirror test asserted the field changed after
      `clay_set_layer_mirror` and failed. Not a cache bug: `Node::mirror`
      defaults to false, so a layer's mirror axes apply only to items that
      opted in. The test now sets the item flag. Worth recording because the
      mirror test added in the previous change checks the lock and the undo
      stack but NOT that mirroring moves the surface — that gap is now closed.
- [x] 4.2 The existing suite already catches a fully disabled invalidation
      (7 cases, 11 assertions) and even the undo/redo-only omission. The new
      suite's value is localisation — it names the entry point that failed —
      and explicit per-entry-point coverage so a future entry point added
      without a bump is caught by a case that describes it.

## Deliberately not done

- Incremental compilation. Adding one stamp still rebuilds the whole tape, so a
  continuous stroke is unchanged. That is the remaining half of this problem and
  needs the tape layout to support appending.
