## Why

Every edit throws away the whole compiled tape. `Document::tape()` is cached on the document revision, so the raycast a host does to place the next dab recompiles from scratch — measured on `main` at 1bd59e4:

| items | instrs | instrs | params | **total** | compile |
|---:|---:|---:|---:|---:|---:|
| 1,000 | 1,999 | 15.6 KiB | 144.5 KiB | 160 KiB | 0.23 ms |
| 10,000 | 19,999 | 156.2 KiB | 1,445.3 KiB | 1.56 MiB | 1.26 ms |
| 50,000 | 99,999 | 781.2 KiB | 7,226.5 KiB | **7.82 MiB** | **7.58 ms** |

An append-only dab adds **~148 bytes** and forces all 7.8 MiB to be re-emitted. This is issue #197, and it is the dominant sculpt pattern: `node N+1` after N unchanged nodes.

The compiler is already shaped for the fix. `compile_list` walks roots left to right emitting item-then-combine, layers chain with the combine emitted *before* each subsequent layer, and mirror/radial copies are emitted inside `emit_item` — so appending to the tail of the last visible SDF layer leaves the prefix **byte-identical**, and every `param_offset` and blob handle in it stays correct. Phase 1 needs **no relocation pass**; that is what makes it much smaller than the issue's architecture suggests.

What blocks it is not the emitter but the invalidation: `touch()` bumps a counter and discards what changed. The information is right there — every command-based edit funnels through one place with the command in hand, and `AddNodeCmd{parent = kNoNode, index = -1}` *is* "append at the tail".

## What Changes

- Extract the tape compiler's running state (`have_acc`, the `info` fold, `bounds`, `gate_reach_`, `last_volume_blob`, `cull_dropped`, `layer_scale_for_gate_`) into a resumable checkpoint taken at the end of a whole-document compile.
- Add a compile entry point that resumes from a checkpoint plus its tape and compiles only appended nodes, returning a tape whose prefix is the reused bytes.
- Teach the document's tape cache to classify its invalidation: a tail append to the last visible SDF layer reuses the prefix; everything else — mid-list insert, remove, move, any parameter edit, undo, redo, replay, layer changes — falls back to a full compile exactly as today.
- Equivalence test: for every append, the reused tape is **bit-identical** to a full recompile of the same document — instrs, params, blob, `info` and `bounds`. Not "within tolerance".
- Benchmark the append path against the full compile at 1k / 10k / 50k items, gated as a ratio. **Measured: 18.9x at 50k** (0.544 ms against 10.3 ms), 16.7x at 10k, 28.8x at 1k.

Not in this change, and deliberately: the **GPU half**. A reused prefix still produces different bytes overall, so the tape still takes a fresh `compile_id` and every backend still re-uploads. Phase 2 (#197's phase 3) makes the tape identity carry a generation plus a dirty range so Metal patches instead of re-uploading and Vulkan skips its full `memcmp`; it is only expressible *because* this change produces a stable prefix. The issue's phase 2 (a fragment cache for mid-document edits) stays unwritten until this one is measured.

**No breaking changes.** `Tape` stays immutable after compilation and `compile_id` keeps its current contract — the cache *produces* tapes from a reused prefix rather than making `Tape` mutable.

## Capabilities

### New Capabilities
None. This is a new requirement on two existing capabilities, not a new surface.

### Modified Capabilities
- `scene-model`: "Tape compilation" gains the guarantee that compiling an appended document reuses the prefix and is bit-identical to a full compile.
- `c-abi`: "A document reuses its compiled tape until it changes" gains the narrower promise that an append reuses what did not change, without weakening "Every mutation is visible to the next read" or the concurrent-reader snapshot guarantee.

## Impact

- `src/scene/tape_build.cpp` — `Compiler` gains a resumable state; `compile_document` keeps its signature and behaviour.
- `include/clay/scene/tape.h` — the new resume entry point and its checkpoint type.
- `bindings/c/clay_c.cpp` — the cache at `:1053` and the edit funnel at `:1767` learn to distinguish an append from a general edit.
- `tests/unit/` — equivalence and staleness regression tests; `benchmarks/bench_main.cpp` — the append benchmark.
- No ABI change, no backend change, no file-format change.
