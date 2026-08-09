# Proposal: let the consumer reach the brick cache

## Why

`brick::BrickCache` is the structure that makes sculpting incremental: an edit
dirties a handful of bricks and only those re-evaluate. It has existed, tested,
since the Dreams-style design was adopted. It is also unreachable from the only
surface a packaged consumer has — `BrickCache` appears nowhere in `bindings/`,
and `tools/build_xcframework.sh` ships `bindings/c/clay.h` plus the kernel
headers as Metal shader source, "not part of the Swift module". The iPad app
therefore cannot use it at all.

So the app re-derives everything on every look. Three separately measured wins
of 6.6–12x sit behind this gate, and the largest of them — a per-brick compile
that re-derives every item's bounds for every brick — is specifically the part
of refill latency that CANNOT be offloaded to Metal, because `eval_bricks` goes
to the GPU while the tape compile stays serial on the CPU.

The brick-cache spec already mandates the design this exposes ("using per-brick
culled tapes from the scene module"). What was missing was the boundary.

## What it is

A mirror of `brick::BrickCache`, one entry point per member, plus the two
primitives anything filling a brick needs and neither of which the ABI had:
dense-grid evaluation with a cull region, and the influence bound an edit
dirties (per node and per layer, reporting the unbounded case rather than
claiming a finite box for it).

Evaluation requests cross as a fixed-layout array element that is byte-for-byte
`brick::BrickRequest`, pinned by `offsetof`/`sizeof` assertions, so a drain is a
copy rather than a transcription that could drift.

## What it is not

**Not a scheduler.** The C++ class states its central contract as "the consumer
owns threading and evaluation", and the ABI keeps it: no refill loop, no thread
pool, no time budget, no ordering policy. The host marks dirty, drains,
evaluates, submits. `clay_brick_cache_eval_requests` takes no cache handle at
all, so it is free-threaded against one const document and a host can fan out
over requests however it likes.

This was the decisive question between the three designs considered. A fused
"refill" call that let the library decide fan-out reads better in a header and
would have been a permanent commitment to a scheduling policy nobody has
measured on the target device — on a published ABI, where a mistake cannot be
taken back.

**Not an atlas.** A design that handed out a shared arena of brick payloads to
avoid a copy per brick was rejected: the arena would live in the binding while
`Brick::values` stayed a per-brick vector, so a memory-constrained device would
pay for every payload twice to save one 1 KiB memcpy per refilled brick.

**Not pyclay.** The Python bindings do not get the cache in this change. It
wants a buffer protocol for the fp16 payloads and a numpy view over the request
array, which is its own change with its own tests. `check_binding_parity.py`
prints it as an outstanding follow-up on every run rather than hiding it as an
exemption — the gate is one-way (pyclay to C), so nothing would have failed and
nothing would have said so either.

## A guard the engine needs and did not have

`BrickCache::mark_dirty` converts a world region to brick coordinates with
`static_cast<int>(std::floor(region.min.x / brick_size))` and then inserts a
tracked entry per brick in the span, with no range check on either. Three floats
from a camera frustum are undefined behaviour followed by an unbounded
allocation — and the library builds `-fno-exceptions`, so the `bad_alloc`
terminates the host app rather than returning.

The boundary therefore validates in 64-bit BEFORE the engine converts anything:
non-finite or empty region, a brick coordinate outside `int32`, or a span above
the batch ceiling are each refused with the cache left exactly as it was.
"Dirty everything tracked" is spelled as the absence of a region rather than a
region carrying an infinity.
