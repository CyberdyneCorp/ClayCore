## Why

Phase 1 (`reuse-the-tape-prefix`) made an appended dab cheap to *compile* — 0.544 ms against 10.3 ms at 50,000 items. It did nothing for the upload, and said so.

The reused tape has different bytes from the one it grew from, so it takes a fresh `compile_id`, and `compile_id` is exactly what a backend keys residency on. Every append is therefore a **guaranteed** miss:

- **Metal** (`metal_backend.cpp:531`) compares `t.id == tape.compile_id`, misses, releases the previous entry and calls `newBuffer(StorageModeShared)` three times — 7.82 MiB re-allocated and re-copied per stamp. On an iPad that is allocator churn on the platform where memory pressure ends sessions.
- **Vulkan** (`vulkan_backend.cpp:490`) compares contents with `memcmp`. An append changes the sizes, so it short-circuits false immediately and re-uploads everything — *and* it keeps `res_instrs_`/`res_params_`/`res_blob_` as a full CPU-side shadow, another 7.82 MiB, for a cache that cannot hit even once during a stroke.

An append adds ~148 bytes. Both backends move ~7.8 MiB to absorb it, every stamp.

Phase 1 is what makes the fix expressible: it produces a **stable prefix**, so "what changed" is a suffix with a known start, not a diff nobody can compute.

## What Changes

- A compiled tape gains a **lineage** beside its identity: the `compile_id` it grew from, and the offset in each of `instrs`/`params`/`blob` at which it stops agreeing with that ancestor. `compile_document_append` already knows all four — the checkpoint *is* the agreement point — and every other compile entry point reports no lineage, exactly as today.
- **`compile_id` keeps its current meaning, unchanged**: process-unique per compile, equal only for byte-identical sections. Lineage is additive. A backend that ignores it keeps working, misses, and re-uploads — correct, just not faster. This is what lets Metal land separately without leaving anything inconsistent.
- **Both GPU backends patch instead of re-uploading**: when a resident tape is the named ancestor, write only the changed suffix — into Vulkan's persistently-mapped buffers, and into the `contents()` of Metal's three shared ones.
- **Vulkan stops keeping a full CPU shadow.** With lineage the `memcmp` is unnecessary for the append case; the shadow shrinks to the identity it needs to recognise its ancestor.
- **Buffers grow with slack.** `ensure()` is exact-fit and reallocates whenever the size grows, so patching alone would still reallocate every stamp. Growth becomes geometric.
- Tests assert what the counters already make visible: a stroke of N dabs performs one upload and N patches, and the field after patching is bit-identical to the field after a full upload.

**Metal was deferred and is no longer.** It was left out of the first pass on purpose: nothing on the machine that pass was written on could measure it, and shipping a performance change whose number nobody has seen is how unverified claims enter a codebase. It was filed as #296 and is now here, written and measured on a Mac and validated on the reference iPad — which is the hardware the claim was always about, since Metal is the iPad app's production path and an iPad is where a per-stamp allocation is paid under memory pressure.

The deferral cost nothing, which is the point of having made `compile_id`'s meaning survive lineage: between the two, Metal ignored the lineage, missed, re-uploaded, and stayed correct.

**Not in this change: the per-brick path.** Culled tapes are compiled per dab against a `CullPlan` and are already 0.13% of a stamp (#197).

## The complication worth stating up front

The two backends do not pack a tape the same way, and only one of them makes an append a tail write.

- **Metal** keeps `instrs`, `params` and `blob` in three separate buffers. An append is a pure tail write into each.
- **Vulkan** packs `params` and `blob` into **one** buffer, params first, with `blob_base = params.size()` as a push constant. So appending an item that adds params **shifts the entire blob region right**. It is a tail write only when the blob is empty.

An ordinary sculpt is the empty-blob case — plain prims carry no blob — so the dominant path is a clean tail write. A document holding strokes, sampled volumes or gates is not, and the design has to say what happens there rather than discovering it in a benchmark.

**Decided: slack between the two regions.** The blob is placed after a params region with spare capacity, and `blob_base` becomes a real cursor rather than `params.size()`, so appended params land in the gap and the blob never moves. It is a host-side change only — no shader edit, no descriptor-set change — and it makes the blob-carrying case a tail write like the empty one. The cost is some unused buffer space and a re-pack when the slack runs out, which is an ordinary full upload and already the path this change is making rare.

Rejected: a separate blob binding, which is cleaner long-term and matches Metal's layout but changes the shader and its descriptor set for a win the slack already gets; and accepting a `memmove` of the blob, which leaves stroke- and volume-heavy documents paying O(blob) per dab — the 7.2 MiB case this issue is about.

## Capabilities

### New Capabilities
None. This adds requirements to two existing capabilities.

### Modified Capabilities
- `evaluation-backends`: "Repeated evaluation reuses device-resident state" gains the lineage a tape may carry, the patching a backend may do with it, and the unchanged-values guarantee that patching is a speed change only.
- `scene-model`: "An appended document reuses its compiled prefix" (added by `reuse-the-tape-prefix`) gains the requirement that the reuse also *reports* where the agreement ends, since that is what makes the prefix usable by anything but the compiler.

## Impact

- `include/clay/scene/tape.h` — the lineage fields beside `compile_id`, and what they promise.
- `src/scene/tape_build.cpp` — `compile_document_append` fills them in; every other entry point leaves them empty.
- `backends/vulkan/vulkan_backend.cpp` — `upload_tape`, `resident`, `remember`, `ensure`.
- `backends/metal/metal_backend.cpp` — `upload_tape`, `upload_section`, `ResidentTape`, and the two counters beside them.
- `tests/device/` — a case that drives a STROKE rather than a stamp-and-undo, since the reset every other latency case uses is itself the invalidation that makes the append path unreachable.
- `tests/unit/test_backend_residency.cpp` and the Vulkan tests — one upload per stroke, and patched output bit-identical to uploaded output.
- No ABI change, no file-format change, and **no shader change** — the slack layout keeps `blob_base` a push constant the shader already reads.
