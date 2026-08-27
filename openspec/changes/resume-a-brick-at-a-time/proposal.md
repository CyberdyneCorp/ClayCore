# Proposal: resume a brick at a time, not a batch at a time

## Why

`resume-the-brick-refill` shipped the seed store that #306 asked for, and it
works: a warm brick evaluates only what the document gained since its seed was
taken. Its admission test, though, is written about the BATCH —

> When the next call asks for bricks that **all** carry a seed from the same
> revision […]

— and that turns out to describe almost no real refill.

Nothing re-stamps a seed except the refill that writes it. `touch_appended`
bumps the revision and deliberately leaves the store alone; a refill re-stamps
only the bricks it filled. So after a dab, the bricks that dab covered sit at
the new revision and every other brick keeps whatever it last got. The next dab
of a stroke that has MOVED — which is every stroke — asks for a window that
mixes the two, and one disagreeing brick sent all of them down the full walk. A
brick the stroke had never reached, or one the byte budget had evicted, did the
same.

The effective precondition was "this batch's bricks are a subset of the last
batch's": the fast path fired only while the brush stood still. Measured on a
four-brick window sliding one brick every third dab, one appended dab:

| history | before | after | worst dab before | worst dab after |
|--------:|-------:|------:|-----------------:|----------------:|
|   1,000 | 0.31 ms | 0.03 ms | 1.04 ms | 0.12 ms |
|   5,000 | 1.10 ms | 0.05 ms | 3.90 ms | 0.39 ms |
|  20,000 | 4.60 ms | 0.15 ms | 19.50 ms | 1.36 ms |
|  50,000 | 9.75 ms | 0.52 ms | 30.41 ms | 3.65 ms |

The tail is the half an artist feels: a 30 ms hitch every time the brush crosses
a brick plane. Both columns are correct — the full walk is the reference — so
this is throughput, not a wrong field.

Two things fell out of reading the same code, and are here because they are the
same function:

**An append to a layer BENEATH the active one could be resumed onto it.**
`plan_resume` picks the last visible SDF layer and then trusts the append log
without checking which layer the log describes, though `touch_appended` records
it. The only remaining guard is `compile_layer_suffix` checking that the
appended ids are the tail of the ACTIVE layer's roots — and a NodeId is only
meaningful inside one layer's content, since every layer's ids start at 1. When
the lower layer's new id equals the active layer's last root, that check passes:
the suffix folds the active layer's own last node onto the seed a second time,
the dab that was actually made is never evaluated, and the brick is marked
answered so the fallback never runs. Reproduced at up to **0.49 in distance**,
ten cells at a 0.05 voxel, silently. Making resume fire per brick makes this
reachable more often, so it is fixed here rather than after.

**The fast path was unobservable.** Resumed and replayed bricks are
bit-identical by contract, so no output can say which produced a brick, and a
fast path that stops firing reads as correct code with a frame time that
quietly follows the size of the sculpt. That is exactly how this shipped: the
existing tests and benchmarks reuse ONE fixed brick window for every dab, so the
moving case measured identically to a hit. `clay_document_resume_stats` reports
it.

## What

**The admission test becomes per brick.** A brick's stored revision is a
property of that brick, so each is carried forward from wherever it is. Plans
are memoized per distinct revision — a moving window holds one or two — and the
suffix compile and evaluation were already per brick against that brick's own
cull region, so the arithmetic is unchanged and stays bit-identical. Bricks that
cannot be served fall into the miss gather that already existed and take the
full path alone.

The lattice-shape checks (`per`, spacing, dims, colour and below presence) move
into one place that both the revision lookup and the seed lookup use. They used
to gate the whole batch; per brick they have to gate the brick, or a mismatched
entry would hand back a buffer of the wrong length.

**`plan_resume` refuses when the appends did not go to the layer the suffix
would extend.** Two members already under the same lock; the plan is simply
unusable otherwise, and the batch takes the correct full path.

**`clay_document_resume_stats` (ABI 0.55.0)** reports seed-store occupancy and
bytes, and cumulative `resumed_bricks` / `refilled_bricks`.

## Non-goals

The device-buffer refill still has no resumed path. Eviction is still FIFO by
first insertion rather than LRU, and `touch_region` still leaves erased keys in
the order deque. The resumed loop still holds the document cache lock across
every compile and evaluation. Each is measured and filed separately; none is
made worse here.
