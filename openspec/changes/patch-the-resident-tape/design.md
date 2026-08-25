## Context

See `proposal.md` — Why. Four facts about the code decide the shape.

**Phase 1 already computed the answer.** `compile_document_append` copies `cached[0 .. checkpoint)` and compiles onto it, so the agreement point between the new tape and its ancestor *is* the checkpoint — exactly, not approximately. Nothing has to be diffed or discovered; the lineage is three numbers the compiler already has in hand.

**The two backends pack a tape differently, and only one makes an append a tail write.** Metal keeps `instrs`, `params` and `blob` in three separate buffers (`metal_backend.cpp`, `upload_section` × 3). Vulkan keeps `instrs` in one buffer and `params ++ blob` in a second (`upload_tape`, `vulkan_backend.cpp:462`), with `blob_base = params.size()` as a push constant — so growing `params` shifts the blob.

**The shader already treats `blob_base` as an opaque cursor.** `clay_kernels.comp.in:27` passes `pc.blob_base` straight into `ctape_eval`; it never assumes the blob begins where params end. Moving the blob is a host-side change with no shader edit and no descriptor-set change.

**Vulkan's buffers are host-visible, host-coherent and persistently mapped** (`ensure`, `vkMapMemory` into `b->mapped`). A patch is a `memcpy` at an offset — there is no staging buffer to route around.

## Goals / Non-Goals

**Goals:**
- An append transfers what changed, not the whole tape, on both GPU backends.
- Vulkan stops keeping a full CPU-side copy of the tape it has already uploaded.
- A stroke does not reallocate device buffers on every stamp.
- A backend that ignores all of this stays correct — which is what let Metal follow separately rather than simultaneously.
- The Metal claim is measured on a Mac and validated on the reference iPad, since Metal is the iPad app's production path.

**Non-Goals:**
- Patching anything but an append. A mid-document edit has no stable prefix and is out of scope for the same reason it was in phase 1.
- Vulkan's single resident slot. It holds one tape where Metal holds several, so alternating pick and eval tapes thrashes it. Pre-existing, unrelated to appends, and a separate change.

## Decisions

### Lineage is three offsets and an ancestor id, produced in exactly one place

`Tape` gains `parent_id` plus `agree_instrs` / `agree_params` / `agree_blob`. `compile_document_append` sets them from the checkpoint it just consumed; every other entry point leaves `parent_id` at 0, which means "no lineage" the same way `compile_id == 0` means "no identity".

One producer is the whole safety argument. Lineage is a claim that two tapes are byte-identical below three offsets, and a backend that patches on a false claim evaluates a field that never existed — silently, with no error and no crash. Deriving it from the checkpoint that was actually used to build the tape makes the claim true by construction rather than by inspection.

*Alternative considered — a backend diffing the tapes itself.* That is what Vulkan's `memcmp` already does, and it costs what the upload costs. The point of lineage is to know without looking.

### `compile_id` keeps its meaning, and that is what made Metal deferrable

Lineage is additive. `compile_id` stays process-unique per compile and equal only for byte-identical sections, so `if (t.id == tape.compile_id)` in `metal_backend.cpp:531` keeps doing exactly what it does now: on an append it misses, releases, and re-uploads. Correct, no faster, no worse than today.

This is deliberate, not incidental. It meant the Vulkan half could land, be measured on the backend that machine could measure, and leave Metal in a working state rather than a half-converted one — which is exactly what happened between the two halves.

### Vulkan keys residency on the id, and stops shadowing the tape

`resident()` becomes an id compare, which is what Metal already does:

- `tape.compile_id == resident_id_` → hit, dispatch as-is.
- `tape.parent_id == resident_id_` and non-zero → **patch** the three suffixes.
- otherwise → upload whole.
- `tape.compile_id == 0` → upload whole, always. A hand-assembled tape has no identity to trust.

The `memcmp` and the `res_instrs_`/`res_params_`/`res_blob_` vectors go with it — 7.82 MiB of CPU-side copy at 50k items, kept for a cache that cannot hit during a stroke. What replaces them is one `std::uint64_t`.

The existing comment argues against a *hash*, and it is right: two different tapes that hashed alike would evaluate the wrong field silently. An id is not a hash. It is stamped by the compiler, process-unique, and never collides by construction — which is the same reasoning Metal has relied on since it was written. Both existing Vulkan residency tests keep passing, and the second gets stricter: the "same length, different contents" tape it moves is now caught by identity rather than by a compare that had to read every byte to find out.

**A patch advances the resident id to the patched tape's own.** A stroke is a chain — tape B extends A, C extends B — so without advancing it, only the first dab after an upload would patch. This is one line and the whole reason a stroke rather than a dab is cheap.

### Metal patches in place, and needs none of the gap Vulkan needed

Metal already keeps `instrs`, `params` and `blob` in three separate `MTL::Buffer`s, so an append is a tail write into each and there is no packing problem to solve. The three things it does need are the three the Vulkan work learned the hard way, and each is a distinct failure:

- **Patch on `parent_id`, not on `compile_id`.** The `compile_id` compare stays first and unchanged; a `parent_id` match is checked next, and a hand-assembled tape (`compile_id == 0`) still goes through the scratch slots and is never served a resident buffer.
- **Advance the resident entry's id to the patched tape's own.** A stroke is a chain, so without this only the first dab after an upload patches and every dab after it re-uploads. One line; the whole difference between a cheap stroke and a cheap first dab.
- **Reserve slack at `newBuffer` time.** An `MTL::Buffer` cannot be resized, so an exact fit means the next dab does not fit, the patch declines and all three sections are re-allocated — the allocation patching exists to avoid, still paid per stamp. The reservation is `n + n/2 + 1024` elements, identical to Vulkan's, so the two backends do not need separate explanations when their numbers are compared.

**Writing into a resident buffer in place is safe here for a reason the file already relies on**: every Metal submit returns only once its command buffer reports completed or errored, and `mutex_` serializes the public entry points, so no resident buffer is being read by the device while a patch runs. That is the same argument the scratch pool's reuse has always rested on.

**The eviction hazard is Metal's alone.** Metal holds four tapes resident under an LRU where Vulkan holds one, so the entry a patch lands in is found by *ancestor id* while the entry a whole upload evicts is found by *least-recent use*. Patch into the wrong slot and an unrelated resident tape — a pick tape, a layer tape — silently becomes the stroke. There is a test for exactly that, and it is Metal-only because the hazard is.

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

**Metal was left behind → ** it was, deliberately, and it no longer is. Between the two halves it ignored lineage and kept its previous behaviour, which is what "correct, merely no faster" was written to guarantee. The follow-up was named in the tasks and filed as #296 rather than left implied, which is what made it get done.

**A Mac measures the wrong half of this on Metal → ** and it does. On unified memory the patched and re-uploaded strokes measure 49.0 ms against 50.2 ms of wall clock — 1.02x, well inside noise — because both evaluate the same 40,000-instruction tape with the same dispatch and differ only in what the host copied first. The numbers that moved are host CPU (2.0 ms against 3.1 ms a dab) and the reallocation count (0 against 300). So the **gate is the counter**, which is exact and machine-independent, and the iPad is where the memory story is checked rather than inferred — a device case that drives a real stroke, because every other latency case resets between iterations and the reset is itself the invalidation that makes the append path unreachable.

## Migration Plan

No ABI, file-format or shader change. Lineage is additive and ignored by default; both backends' patching is internal. Rollback is making the patch path decline — `resident()` returning false on Vulkan, `can_patch` returning false on Metal — which restores upload-per-append on either backend independently.

## Open Questions

- ~~How much slack, and whether it should scale with the document or be a fixed reserve.~~ Settled by measurement: **both terms**, `n + n/2 + 1024`. The proportional half alone starves a small document — an item is ~37 params, so a 50% reserve on very little is not room for one dab — and the constant half alone re-packs a large one linearly. Together the re-packs over a stroke are geometric: 0 over the 300-dab benchmark and 8 over 8,154 appends. The same reservation is used on both backends.
