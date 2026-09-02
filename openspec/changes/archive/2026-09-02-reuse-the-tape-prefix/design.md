## Context

See `proposal.md` — Why. Three facts about the existing code shape this design.

**Emission is already append-ordered.** `compile_list` (`src/scene/tape_build.cpp:716`) walks roots left to right emitting item-then-combine; layers chain with `emit_combine(Op::Add)` emitted *before* each subsequent layer and nothing after the last; mirror and radial copies are emitted inside `emit_item`. So a tail append leaves the prefix byte-identical and every `param_offset` and blob handle in it already correct. **No relocation pass is needed** — this is the "append-only invariance" branch of #197's design note, and it is what makes this change small.

**The compiler's running state is smaller than it looks.** Most of it lives in `Tape` itself (`instrs`, `params`, `blob`, the `info` fold, `bounds`). Of the three members outside it, **none needs checkpointing**: `last_volume_blob` (`:225`) and `gate_reach_` (`:321`) are written and read within one item's iteration, and `layer_scale_for_gate_` (`:317`) is re-set at every `compile_list` entry. `cull`/`cull_test`/`index`/`plan` are all null on the whole-document path, and `scratch_curve` is pure scratch. What genuinely crosses items is only the two accumulator flags `run()` and `compile_list` thread.

**The invalidation point knows what happened, and throws it away.** Every command-based edit passes `scene::apply(doc, cmd)` then `doc->touch()` at `bindings/c/clay_c.cpp:1767`, with `cmd` in hand. `AddNodeCmd{layer, parent = kNoNode, index = -1}` (`include/clay/scene/commands.h:21`) is exactly "append at the tail of the layer's root list".

**Measured on `main` at 1bd59e4, 50k items:** full compile 7.58 ms; copying the 7.82 MiB prefix and appending one item's worth 0.83 ms, at 9.2 GiB/s. That ratio — **9.1x** — was taken as the honest ceiling, and the reason it is a ceiling is the subject of the first decision below.

**What it actually landed at: 18.9x** (0.544 ms against 10.3 ms, quiet box, 3 repetitions). The estimate was conservative because it measured a prefix copy that grew by `push_back`; the implementation `assign`s into empty vectors, which is one allocation and one copy. The *shape* of the estimate holds — this is bandwidth-bound, not compile-bound — and it is still the reason the ceiling exists at all.

## Goals / Non-Goals

**Goals:**
- Make the whole-document compile proportional to what was appended rather than to the document, for the tail-append case.
- Keep the reused tape bit-identical to a full compile, and prove it by test rather than by argument.
- Make the fast path opt-in at each call site, so code that does not know it is appending stays correct by construction.

**Non-Goals:**
- The GPU re-upload. A reused prefix still yields different overall bytes, so the tape still takes a fresh `compile_id` and Metal still misses residency. That is phase 2 and it is the bigger prize; this change exists partly to make it expressible.
- The culled per-brick path. Per-brick tapes are compiled per dab against a `CullPlan` and are already 0.13% of a stamp (#197); resuming them buys nothing and the cull's per-chain `cull_dropped` state would have to be checkpointed too.
- Mid-document edits, a fragment cache, and checkpoints — #197's phase 2, which the issue itself says to defer until this is measured.

## Decisions

### Reuse copies the prefix; it does not mutate a tape in place

`Tape` is immutable after compilation and `compile_id`'s contract depends on that — two tapes with the same nonzero id carry byte-identical bytes, which is what lets Metal keep an upload resident without hashing. Readers hold `shared_ptr<const Tape>` snapshots that must stay valid for the whole call (`c-abi` — "A document stays readable from several threads at once"). So the resumed compile **copies** the prefix into a fresh `Tape`, appends, and stamps a new id.

That is why the win is 9.1x and not 400x: this trades an O(N) *compile* for an O(N) *memcpy*. It removes the per-item work — influence bounds, transform inversion, curve tessellation, info folding — but not the byte movement.

*Alternative considered — move the prefix instead of copying it* when the cache slot is the sole owner (`use_count() == 1` under the cache mutex, so no reader holds a snapshot). Sound, and it would take 0.83 ms to roughly nothing. Deliberately **not** in this change: it makes correctness depend on a refcount observation rather than on the tape being immutable, and it should be added as a measured refinement on top of a version that is already proven bit-identical, not folded into the change that establishes the invariant.

*Alternative considered — return a tape that shares the prefix's storage* (a rope, or a `shared_ptr` to a prefix segment). Rejected: every backend and every evaluator takes `instrs.data()` as one contiguous array, and `param_offset` is an absolute index into one `params` buffer. Segmenting the tape is a change to the kernel ABI, which is phase 2's territory and much larger than this.

### `touch()` stays the conservative default; the append path is a separate, narrower call

The fast path is **not** inferred by comparing documents. `touch()` keeps meaning "something changed, recompile everything", and a new `touch_appended(layer, node_ids)` records an append delta instead. Every existing `touch()` call site — undo (`:2521`), redo (`:2535`), event replay (`:2398`), consolidation (`:4201`), `add_sdf_layer` (`:5268`) — keeps today's behaviour with no edit, and any mutating entry point added later is safe by default.

This is the whole safety argument. Reusing a prefix that has in fact moved is silent: the call succeeds, nothing errors, and every later read answers with a wrong field. The spec states it as "where it is not obvious the compiler SHALL compile in full", and making the unsafe thing the one you have to ask for by name is how that gets enforced rather than remembered.

*Alternative considered — a structural check* that walks the document and confirms it matches the compiled prefix. Rejected: it is O(items) per edit, which is the cost being removed, and it would still have to prove per-node equality to be sound.

### Appends accumulate as a delta log, consumed at the next compile

A host may append several dabs before reading. The cache holds, alongside the tape and its revision, a small log of node ids appended since that tape was compiled. `touch_appended` pushes onto it; `touch()` clears it and marks the tape fully stale. A rebuild with a non-empty log and a live prefix resumes and compiles the logged nodes in order; anything else compiles in full. The log is guarded by the existing `cache_mutex_`, so recording an edit and rebuilding cannot interleave.

### Reuse requires the append to target the layer the compile ended in

`compile_document` emits visible SDF layers in order. The prefix is only a prefix if the appended nodes still emit last, so the checkpoint records which layer the compile ended in, and reuse is refused unless every logged append targets that same layer and it is still the last visible SDF layer. A layer added, removed, hidden or shown goes through `touch()` and clears the log anyway; the recorded layer is what catches an append to an *earlier* layer while a later one exists.

### The checkpoint is a truncation point, not the end of the tape

This is the correction to make before coding, found by reading `run()` rather than `compile_list`. Layers do **not** simply concatenate. `run()` compiles each visible SDF layer's chain with its own fresh accumulator and then folds the result into the layers below with a hard union:

```
bool layer_val = compile_list(layer.sdf->roots, ..., /*have_acc=*/false);
if (!layer_val) continue;
if (have_acc) emit_combine(Op::Add, Blend{}, 0.0f);   // layers union hard
have_acc = true;
```

So with more than one visible SDF layer the tape ends with that union, and an appended item is **not** a tail append onto the cached bytes. Emitting it after the union would combine it against every layer — which for a hard-blended `Add` is invisible, because `min` is associative, and for a smooth blend or a `Subtract` is simply the wrong field. A bug that only shows up in multi-layer documents with a soft brush is exactly the silent kind this change must not introduce.

The fix keeps the reuse a genuine byte prefix. The checkpoint records the tape lengths **at the end of the last visible SDF layer's chain, before that union**, plus the two flags: `layer_have_acc` (the chain's own accumulator, which is what a resumed `compile_list` must be handed) and `doc_have_acc` (whether an earlier layer put a value underneath). Resuming is then: copy `cached[0 .. checkpoint)`, run `compile_list` over the appended ids with `layer_have_acc`, and re-emit the union if `doc_have_acc`. Single-layer documents fall out as the case where the checkpoint is the whole tape and there is no union to re-emit; an empty last layer falls out as `layer_have_acc == false`.

`info` and `bounds` are taken from the cached tape unchanged, because the layer union folds neither — a hard `Add` is exact and adds no extent.

`compile_document` keeps its exact signature and behaviour — it is on the per-brick path in `clay_brick_cache_eval_requests` and in the golden-corpus tests, and neither should change. The resumable form is a separate entry point that returns the checkpoint beside the tape, and the resuming form validates the checkpoint against the document and refuses rather than trusting it.

## Risks / Trade-offs

**The win is bounded by memcpy, not by the compile → ** 18.9x measured at 50k (0.544 ms against 10.3 ms), not the two orders the instruction count suggests. Recorded so the benchmark is judged against a copy rather than against zero, and so the move-when-sole-owner refinement has a stated baseline to beat.

**The two sides of the ratio gate do not share a bottleneck → ** found while setting it. Reuse is bandwidth-bound and the full compile is compute-bound, so a loaded runner moves them apart rather than together: with two other jobs at ~98% CPU on this box the ratio degraded from 0.053x to 0.31x while the compile side barely moved. The gate is set at 0.50 — well above the worst contention seen, still 2x below the 1.0x a lost fast path reads — and the reasoning is recorded in `check_bench.py` so a CI failure is not misread as a regression.

**A silently wrong prefix is the failure mode → ** the bit-identical equivalence test is the gate, and it runs over the golden corpus plus a generated document, with the appended-to layer carrying a mirror, a radial symmetry, a mask and a transform, and the appended nodes carrying blends, groups, strokes, sampled volumes, gates and deformer chains. `info` and `bounds` are compared as well as the three byte arrays — a tape that matches byte-for-byte but folded a different Lipschitz bound would evaluate right and step wrong.

**`Compiler` is one struct doing emission and traversal; pulling state out of it can change what an existing path emits → ** the equivalence test above covers the whole-document path, and the existing per-brick culled-tape equivalence tests cover the path this change must leave alone. Both run before the fast path is enabled anywhere.

**Two invalidation entry points can drift → ** mitigated by direction: the narrow one is only reachable from `apply_edit` with an `AddNodeCmd` in hand, and everything else keeps calling the conservative one. A miss costs a recompile, which is today's behaviour.

**Undo after an append must not resurrect the log → ** undo calls `touch()`, which clears it. Covered by a regression scenario in the `c-abi` delta.

## Migration Plan

No ABI, file-format or backend change; nothing to migrate. The fast path is additive and the fallback is the current code, so it can be disabled by making `touch_appended` forward to `touch()` if anything is found wrong in the field.

## Open Questions

- Whether a group subtree appended at the tail is worth admitting to the fast path in this change or deferring to phase 2. It is a legitimate tail append and `compile_group` emits in place, so it should work; the question is only whether the test matrix for grouped appends is worth carrying now. Resolvable during implementation without changing the specs or the approach — the conservative answer, refusing subtree appends, is a subset of the stated behaviour.
