# Tasks: reuse the tape prefix

## 1. Establish the shape before building

- [x] 1.1 Confirmed on `main` at 1bd59e4 that neither half of #197 has moved:
      `Document::tape()` (`clay_c.cpp:1053`) still keys on the revision,
      `compile_document` still stamps a fresh `compile_id`, and Metal
      (`metal_backend.cpp:531`) still keys residency on it.
- [x] 1.2 Re-measured the cost. 50k items: 99,999 instrs, 7.82 MiB, 7.58 ms per
      compile. Params are 90.2% of the bytes (~148/item), reproducing the
      issue's table exactly.
- [x] 1.3 Verified the emitter is append-ordered — `compile_list` left to
      right, layer combines emitted BEFORE each subsequent layer, mirror and
      radial copies inside `emit_item`. So the prefix is byte-identical on a
      tail append and **no relocation pass is needed**.
- [x] 1.4 Measured the ceiling this change can reach: copying the 7.82 MiB
      prefix and appending is 0.830 ms at 9.2 GiB/s, so **9.1x**, bounded by
      memcpy rather than by the compile. Judge task 5 against 0.83 ms.
- [x] 1.5 DECIDED (design Open Question): group subtrees DO join the fast path.
      `compile_group` emits in place and its empty-subtree rollback only
      resizes away what it just appended, so a tail-appended group is a tail
      append by the same argument as an item. Admitting it is also less code
      than refusing it, which would need `is_group` special-cased in the
      funnel. Covered by 4.2.
- [x] 1.6 FOUND while reading `run()`, and it corrects the design: layers do
      NOT concatenate. Each visible SDF layer compiles with its own fresh
      accumulator and is folded into the layers below by a hard union emitted
      AFTER its chain. So with >1 visible SDF layer the cached tape ends in
      that union and an append is not a tail append onto it. The checkpoint is
      therefore a TRUNCATION POINT — end of the last layer's chain, before the
      union — and the union is re-emitted after the appended nodes. Design
      updated; see "The checkpoint is a truncation point".
- [x] 1.7 FOUND: none of `last_volume_blob`, `gate_reach_` or
      `layer_scale_for_gate_` needs checkpointing. The first two are written
      and read inside one item's iteration; the third is re-set at every
      `compile_list` entry. The checkpoint is the three tape lengths plus two
      flags.

## 2. Make the compiler resumable

- [x] 2.1 Add the `TapeCheckpoint` value: the three tape lengths at the end of
      the last visible SDF layer's chain, that layer's id, `layer_have_acc`
      and `doc_have_acc`. Per 1.6 this is a truncation point, not the end of
      the tape; per 1.7 no other compiler member belongs in it.
- [x] 2.2 Add the entry point that returns a checkpoint beside the tape, and
      the one that resumes: copy `cached[0 .. checkpoint)`, compile the
      appended ids with `layer_have_acc`, re-emit the layer union if
      `doc_have_acc`. It VALIDATES the checkpoint against the document and
      refuses rather than trusting it.
- [x] 2.3 Leave `compile_document` byte-for-byte identical in signature and
      behaviour — it is on the per-brick path and in the golden-corpus tests.
      The existing culled-tape equivalence tests are the guard.
- [x] 2.4 Keep `Tape` immutable and give the resumed tape its own nonzero
      `compile_id`. Do NOT inherit the prefix's id: the bytes differ, and the
      id's whole contract is that equal id means equal bytes.

## 3. Teach the cache what an append is

- [x] 3.1 Add `touch_appended(layer, node_ids)` beside `touch()` and leave
      `touch()` meaning "recompile everything". Every existing call site —
      undo, redo, replay, consolidation, `add_sdf_layer` — keeps today's
      behaviour untouched.
- [x] 3.2 Call it from `apply_edit` (`clay_c.cpp:1767`) only for
      `AddNodeCmd{parent == kNoNode, index == -1}`; every other command keeps
      calling `touch()`.
- [x] 3.3 Hold the appended node ids in a delta log beside the cached tape,
      guarded by the existing `cache_mutex_`, so several dabs before a read all
      resume onto one prefix. `touch()` clears the log.
- [x] 3.4 Refuse reuse unless every logged append targets the layer the
      checkpoint ended in AND that layer is still the last visible SDF layer.
- [x] 3.5 Leave `pickable_tape()` and `cull_index()` on the revision alone —
      this change touches one slot.

## 4. Prove it is the same tape

- [x] 4.1 Equivalence test: for every append, the reused tape is
      **bit-identical** to a full compile — `instrs`, `params`, `blob`, AND
      `info` and `bounds`. A tape that matches byte-for-byte but folded a
      different Lipschitz bound evaluates right and steps wrong.
- [x] 4.2 Run that over the golden corpus plus a generated document, with the
      appended-to layer carrying a mirror, a radial symmetry, a mask and a
      layer transform, and the appended nodes carrying blends, strokes,
      sampled volumes, gates and deformer chains.
- [x] 4.3 Regression test for the fallbacks: insert before the end, remove,
      move, edit in place, append to a non-last layer, hide the last layer —
      each compiles in full and matches a fresh compile.
- [x] 4.4 Regression test for staleness, which is the silent failure: append,
      read, append, read — each read reflects every append so far.
- [x] 4.5 Regression test that undo after an append clears the log and reads
      exactly as it did before, and that redo restores it.
- [x] 4.6 Concurrency test: readers evaluating and picking while a thread
      appends never observe a partially rebuilt tape.

## 5. Measure

- [x] 5.1 Benchmark the append rebuild against the full compile at 1k / 10k /
      50k items, and gate it. Named, not `Arg()`-parameterised —
      `check_bench.py` keys on the name before "/".
- [x] 5.2 Report the ratio against 1.4's 0.83 ms baseline. If it is materially
      worse than 9x at 50k, something is copying twice.
- [x] 5.3 State plainly in the benchmark comment that this leaves the GPU
      re-upload untouched, so the next person does not read the CPU win as
      having closed #197.

## 6. Documentation

- [x] 6.1 Update the `Tape` header contract to say what a resumed compile
      guarantees and what it does not.
- [x] 6.2 Comment on #197 with the measured result, and say explicitly that
      phase 2 — the tape identity carrying a generation and a dirty range so
      Metal patches instead of re-uploading and Vulkan skips its `memcmp` — is
      what actually closes the issue.
