# Proposal: a brick refill should resume, not replay

## Why

#308 built the way out of a dab costing what the artist has already sculpted:
compile only the appended items and run them onto the value the rest of the
chain produced, which is bit-identical rather than approximate. It shipped as a
mechanism with no caller, because nothing kept the value.

This keeps it. What a brick refill hands back IS that accumulator — exact, in
float32, at the brick's own lattice — so the only thing missing was somewhere to
put it.

## What

`clay_brick_cache_eval_requests` keeps its results as per-brick seeds. When the
next call asks for bricks that all carry a seed from the same revision, and the
document has only been APPENDED to since, it compiles the appended items per
brick and evaluates them onto those seeds instead of walking the whole surviving
edit list over every sample.

Three things had to be true for that to be exact, and each is now gated:

**The suffix is culled as a whole-document compile would cull it.** A suffix
culled differently from the prefix it continues is a different field — and only
outside the band, which is where nothing is looking. `compile_layer_suffix` now
takes a cull region and the document's pad.

**The cull pad has not changed.** The pad decides which items a brick's compile
keeps, so a seed taken under a different one was continued from a different
field. The pad only grows on an append, so this is a real gate.

**The brick's prefix produced an accumulator.** A brick whose whole chain the
cull dropped has none, and a suffix compiled as though it had one would combine
against far-outside instead of seeding the chain. Read off the stored values: an
empty tape evaluates to `CLAY_TAPE_FAR` everywhere.

Distances only, and one visible SDF layer. A seed is one float a sample, so a
caller asking for colour takes the full path; and with a second layer the seed
may sit under a union it does not describe.

**The append log became multi-consumer.** It used to reset when a reader
absorbed it, so whichever cache the host asked for first spent it — #309 worked
around that with a second copy, and this would have wanted a third. An append
bumps the revision by exactly one, so entry i IS the append from `base + i` to
`base + i + 1`, and readers at different revisions each take their own tail.

## Impact

Through the public ABI, 12 bricks on the pole of a sculpted sphere, one dab:

| edit-list length | before | after | |
|---:|---:|---:|---:|
| 1,000 | 0.373 ms | **0.037 ms** | 10x |
| 5,000 | 3.525 ms | **0.038 ms** | 93x |
| 20,000 | 7.133 ms | **0.131 ms** | 54x |

The cost stops following the document.

## Memory

16,384 bricks, evicted oldest-first — 32 MB for a dim-8 cache, which is a
stroke's working set several times over. Bricks rather than bytes because every
brick of one cache is the same lattice, so a count is a byte budget stated in
the unit the caller thinks in. The whole store is dropped on any edit that is
not an append, since a seed cannot be carried across one.

## Non-goals

**The device-destination refill.** `clay_brick_cache_eval_requests_device`
writes where this cannot read back, so it keeps the full path.

**Colour.** A seed is one float a sample.

**Deciding what to seed from anything but a refill.** The refill's own output is
the accumulator; nothing else in the ABI produces it.
