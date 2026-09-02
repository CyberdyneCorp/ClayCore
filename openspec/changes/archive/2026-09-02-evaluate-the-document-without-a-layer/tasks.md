# Tasks

## 1. The compile

- [x] 1.1 Replace `run_part`'s `bool below` with a three-way selection —
      before / only / except — keeping the two existing cases byte-identical,
      and thread it through `compile_document_part` and
      `compile_document_part_resumable`. The existing `bool` overloads stay so
      the brick refill's call sites do not move.
- [x] 1.2 The except case emits every visible SDF layer whose id is not the
      named one, in document order, joined by the same hard `Op::Add`, and
      takes no checkpoint — there is no active half for a suffix to resume.
- [x] 1.3 Test in `test_layer_prefix_tape.cpp`: an excluded middle layer;
      except + only summing to the whole under `min`; the same under a cull
      region, where the pad is the document's; and excluding the only visible
      layer giving empty space.

## 2. The C ABI

- [x] 2.1 `clay_eval_points_excluding`, `clay_eval_gradients_excluding` and
      `clay_brick_cache_eval_requests_excluding`, mirroring the whole-document
      forms argument for argument with one `clay_layer_id excluded` added.
- [x] 2.2 An unknown layer is `CLAY_ERROR_NOT_FOUND` with nothing written; a
      hidden or non-SDF layer succeeds. Same count ceilings, same backend
      selection, same fixed brick slots.
- [x] 2.3 Document in `clay.h` what the calls are FOR — the composition rule
      (hard union, so `min` is exact), that they take no edit and are safe
      inside a transaction, and why a stale id is refused rather than ignored.
- [x] 2.4 Test in a C ABI suite: composition against the whole-document form;
      a stale id refused; a hidden layer accepted; brick slots at the
      documented stride; and that a transaction opened on the layer still
      commits after the excluded evaluation.

## 3. Bindings and docs

- [x] 3.1 Keep the binding parity gate green — the Python bindings either gain
      the three calls or the gate's exemption list records why not.
- [x] 3.2 `docs/05-claycore-library.md`: what a host previewing one layer does
      with these, and the composition rule.
