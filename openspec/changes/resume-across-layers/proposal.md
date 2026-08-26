# Proposal: a resumed refill should span layers

## Why

The resumable brick refill made a dab cost what the dab adds, and then carried
colour — but it still refused whenever more than one SDF layer was visible, and
layers are how sculpting is organised.

The refusal was real rather than lazy. A document's visible SDF layers hard-union
left to right, so at the point an append resumes from, the tape holds TWO
accumulators: the layers BENEATH the active one, and the active layer's own
chain. A refill's single output is their union, and a union cannot be taken apart
into the two again.

Seeding from that union is exact only when every appended item is a hard `Add`,
because `min` is associative. A sculpting dab usually carries a smooth blend, and
then `smin(min(D, L), item)` is not `min(D, smin(L, item))`. So the fix is to
stop conflating them.

## What

**`scene::compile_document_part`** emits one side of the split: the visible SDF
layers before `active`, or `active` alone. Both cull under the WHOLE DOCUMENT's
pad — a part compiled under a smaller pad drops items the whole compile keeps,
and then the halves no longer sum to the whole.

**The seed becomes two values per sample.** The half beneath is static across a
stroke — only the active layer moves — so it is stored once and carried forward
untouched; the suffix folds into the active half; the refill applies the union
itself, with the same hard `Add` the whole-document compile emits between layers,
through the kernel's own combine rather than a `min` written out again.

**The suffix is compiled with `doc_have_acc` false**, so it emits no union of its
own. Nothing in the evaluator changed.

**The cold path runs two batched passes** rather than one, and neither is extra
work: the items partition between the halves, so the two together are what the
one pass evaluated, plus a per-sample union.

## Impact

12 bricks on the pole, one dab on the UPPER of two populated layers:

| edit-list length | before | after | |
|---:|---:|---:|---:|
| 2,000 | 1.642 ms | **0.039 ms** | 42x |
| 10,000 | 4.179 ms | **0.054 ms** | 77x |
| 40,000 | 16.410 ms | **0.171 ms** | 96x |

## What still falls back

An edit to a layer BENEATH the active one — it is not an append to the layer a
suffix extends, so every seed goes and the refill is full. Also everything the
earlier changes already listed: a non-append edit, a moved cull pad, a brick
whose active chain produced no accumulator, a colour asked for that the seed did
not keep.

## Non-goals

**Appending to a layer that is not the last visible one.** The tape emits layers
in order and an item added to a middle layer has the layers after it to be folded
in front of; that is a different checkpoint, not a different seed.

**The device-destination refill**, which writes where this cannot read back.
