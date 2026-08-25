## Context

See `proposal.md` — Why. Four facts about the code decide the shape.

**Phase 1 already computed the answer.** `compile_document_append` copies `cached[0 .. checkpoint)` and compiles onto it, so the agreement point between the new tape and its ancestor *is* the checkpoint — exactly, not approximately. Nothing has to be diffed or discovered; the lineage is three numbers the compiler already has in hand.

**The two backends pack a tape differently, and only one makes an append a tail write.** Metal keeps `instrs`, `params` and `blob` in three separate buffers (`metal_backend.cpp`, `upload_section` × 3). Vulkan keeps `instrs` in one buffer and `params ++ blob` in a second (`upload_tape`, `vulkan_backend.cpp:462`), with `blob_base = params.size()` as a push constant — so growing `params` shifts the blob.

**The shader already treats `blob_base` as an opaque cursor.** `clay_kernels.comp.in:27` passes `pc.blob_base` straight into `ctape_eval`; it never assumes the blob begins where params end. Moving the blob is a host-side change with no shader edit and no descriptor-set change.

**Vulkan's buffers are host-visible, host-coherent and persistently mapped** (`ensure`, `vkMapMemory` into `b->mapped`). A patch is a `memcpy` at an offset — there is no staging buffer to route around.

## Goals / Non-Goals

**Goals:**
- An append transfers what changed, not the whole tape.
- Vulkan stops keeping a full CPU-side copy of the tape it has already uploaded.
- A stroke does not reallocate device buffers on every stamp.
- A backend that ignores all of this stays correct, so Metal can follow separately.

**Non-Goals:**
- Metal. Its layout makes the patch straightforward, but nothing here can measure it; it follows on a Mac using the lineage this change defines.
- Patching anything but an append. A mid-document edit has no stable prefix and is out of scope for the same reason it was in phase 1.
- Vulkan's single resident slot. It holds one tape where Metal holds several, so alternating pick and eval tapes thrashes it. Pre-existing, unrelated to appends, and a separate change.

## Decisions

### Lineage is three offsets and an ancestor id, produced in exactly one place

`Tape` gains `parent_id` plus `agree_instrs` / `agree_params` / `agree_blob`. `compile_document_append` sets them from the checkpoint it just consumed; every other entry point leaves `parent_id` at 0, which means "no lineage" the same way `compile_id == 0` means "no identity".

One producer is the whole safety argument. Lineage is a claim that two tapes are byte-identical below three offsets, and a backend that patches on a false claim evaluates a field that never existed — silently, with no error and no crash. Deriving it from the checkpoint that was actually used to build the tape makes the claim true by construction rather than by inspection.

*Alternative considered — a backend diffing the tapes itself.* That is what Vulkan's `memcmp` already does, and it costs what the upload costs. The point of lineage is to know without looking.

### `compile_id` keeps its meaning, and that is what makes Metal deferrable

Lineage is additive. `compile_id` stays process-unique per compile and equal only for byte-identical sections, so `if (t.id == tape.compile_id)` in `metal_backend.cpp:531` keeps doing exactly what it does now: on an append it misses, releases, and re-uploads. Correct, no faster, no worse than today.

This is deliberate, not incidental. It means the phase can land, be measured on the backend this machine can measure, and leave Metal in a working state rather than a half-converted one.

### Vulkan keys residency on the id, and stops shadowing the tape

`resident()` becomes an id compare, which is what Metal already does:

- `tape.compile_id == resident_id_` → hit, dispatch as-is.
- `tape.parent_id == resident_id_` and non-zero → **patch** the three suffixes.
- otherwise → upload whole.
- `tape.compile_id == 0` → upload whole, always. A hand-assembled tape has no identity to trust.

The `memcmp` and the `res_instrs_`/`res_params_`/`res_blob_` vectors go with it — 7.82 MiB of CPU-side copy at 50k items, kept for a cache that cannot hit during a stroke. What replaces them is one `std::uint64_t`.

The existing comment argues against a *hash*, and it is right: two different tapes that hashed alike would evaluate the wrong field silently. An id is not a hash. It is stamped by the compiler, process-unique, and never collides by construction — which is the same reasoning Metal has relied on since it was written. Both existing Vulkan residency tests keep passing, and the second gets stricter: the "same length, different contents" tape it moves is now caught by identity rather than by a compare that had to read every byte to find out.

**A patch advances the resident id to the patched tape's own.** A stroke is a chain — tape B extends A, C extends B — so without advancing it, only the first dab after an upload would patch. This is one line and the whole reason a stroke rather than a dab is cheap.

### The blob sits after a gap, so appended params never move it

The floats buffer becomes `[ params … | slack | blob … ]`, and `blob_base` becomes a real cursor — the params *capacity*, not `params.size()`. Appended params land in the gap; the blob stays where it was uploaded.

An ordinary sculpt never exercises this: plain prims carry no blob at all, so the region is empty and an append is trivially a tail write. It matters for documents holding strokes, sampled volumes or gates, which is where the 7.2 MiB of params in #197's table actually lives.

When the slack runs out — params grow past the reserved capacity — the tape is uploaded whole and re-packed with fresh slack. That is an ordinary full upload, which is the path this change is making rare rather than a path it has to avoid.

*Alternative considered — a separate blob binding*, matching Metal's three buffers. Cleaner long-term and it would make the two backends reason alike, but it changes the shader and its descriptor set for a win the slack already gets. Worth doing when something else needs that binding.

*Alternative considered — `memmove` the blob right by the appended params count.* Smallest diff, and it leaves exactly the documents this issue is about paying O(blob) per dab.

### Buffers grow geometrically, because otherwise patching changes nothing

`ensure()` keeps a buffer only when `b->size >= bytes` and is exact-fit otherwise, so every append that grows the tape destroys and recreates the buffer — the allocation the patch exists to avoid, still paid, every stamp. Growth becomes geometric, and the slack above is reserved out of the grown capacity rather than on top of it.

## Risks / Trade-offs

**A false lineage claim is silent → ** the one producer above, plus a test that reads the ancestor's sections and asserts they really are byte-identical below the three offsets. Not "the field looks right" — the bytes.

**A patched buffer and a re-uploaded buffer could diverge over a long stroke → ** the strongest test is not counting patches but comparing fields: evaluate the same appended document through a backend that patched its way to it and through one that uploaded it whole, and require identical values. A patch that wrote to the wrong offset passes a counter test and fails this one.

**Dropping the shadow copy removes a safety net that was never load-bearing → ** it protected against a *hash* collision, and there is no hash. The id it is replaced with is what Metal has always used.

**Slack wastes device memory → ** bounded, and set against what it saves: re-uploading 7.82 MiB per stamp. The re-pack path keeps it from growing without limit.

**Metal is left behind → ** stated in the proposal and enforced by the design: it ignores lineage and keeps today's behaviour. The risk is that it stays behind, so the follow-up is named in the tasks rather than left implied.

## Migration Plan

No ABI, file-format or shader change. Lineage is additive and ignored by default; Vulkan's patching is internal. Rollback is making `resident()` return false for the patch case, which restores upload-per-append.

## Open Questions

- How much slack, and whether it should scale with the document or be a fixed reserve. Decidable from the benchmark once patching works — the trade is device memory against how often the re-pack path runs, and both are measurable. It does not change the specs, the layout, or the task breakdown.
