# Proposal: resume the device-destination brick refill

## Why

`clay_brick_cache_eval_requests` keeps each brick's float32 result as a seed and
evaluates only what the document gained since (#306, #308, #342, #343). Its
device-buffer sibling did none of that:

```
$ sed -n '/^clay_result clay_brick_cache_eval_requests_device/,/^}/p' bindings/c/clay_c.cpp \
    | grep -cE "plan_resume|seed_for|store_seeds|shaped_entry"
0
```

It validated, bounds-checked the device buffers and handed the whole batch to
`eval_requests_in_chunks` — every dab walking the whole surviving edit list over
every sample, exactly as the host path did before #306.

This is the path a **renderer** uses. `clay_device_adopt` exists so evaluation
lands in the caller's own GPU buffer instead of crossing host memory, so the
callers most likely to be latency-bound were the ones on the slowest route in
the library, and the gap grew with the sculpt. Measured on an RTX 5060, one
appended dab over a 12-brick window:

| history | before | after |
|--------:|-------:|------:|
|   5,000 | 15.23 ms | 0.08 ms |
|  20,000 | 59.25 ms | 0.27 ms |

Both columns are correct — the full walk is the reference — so this is
throughput, not a wrong field.

## What

**The seed store is the document's, and both refills use it.** A brick that can
be resumed is answered on the host by the same code the host-memory refill runs
— one `resume_one_brick`, not a second copy of the arithmetic — and the finished
lattice is **written** into that brick's own slot in the caller's allocation. A
brick that cannot is evaluated on the device into that slot as before, and
**read back** so it becomes the next dab's seed.

Both directions are per **contiguous run** of bricks rather than per brick: the
slots are the documented fixed stride, so consecutive bricks are consecutive
bytes, and a moving window is one or two transfers a dab.

**Two new backend primitives**, `write_device_buffer` and `read_device_buffer`,
with `caps().device_copy` reporting which backends have them. Both default to
`Status::Unsupported` and the capability defaults to false, so a backend without
them takes the whole-batch full walk it took before — correct, silent, and
exactly as fast as it always was. The capability is REPORTED rather than probed
because the refill has to decide before it writes anything: a discovery made
half way through would leave part of the batch resumed and part not.

**Vulkan implements them with a compute shader**, not `vkCmdCopyBuffer`. A
caller lends us a buffer it created for its own renderer and nothing obliges it
to have asked for `VK_BUFFER_USAGE_TRANSFER_SRC` or `_DST`; the storage binding
the evaluation path already requires is the one usage that can be assumed, and a
transfer command on a buffer created without those flags is undefined behaviour
in the caller's own process. One extra SPIR-V module, one extra pipeline, and
binding 2 gains the same caller-override bindings 3 and 4 already had.

**Seeds are kept only where this path can say what they mean.** With more than
one visible SDF layer a seed is two values — the active layer's, and the hard
union of everything beneath it — and this path evaluates the document whole, so
what it could store is neither half. It stores nothing rather than something
mislabelled: the shape gate in `shaped_entry` would refuse such an entry to a
multi-layer reader, but a document that later lost a layer would find it
acceptable and wrong. Multi-layer documents take the full walk here; the
host-memory form keeps the halves apart and resumes them fine.

## The shape this did not take

The issue named a second shape: the seed becomes device-resident, so the suffix
evaluates on the GPU and the accumulator updates with nothing crossing. It was
measured before it was declined, because whether residency pays depends on the
dirty-window size a host actually submits. It does not pay:

| what | cost |
|------|-----:|
| one device dispatch, 1 brick, emptiest possible tape | 23 µs |
| one device dispatch, 12 bricks | 283 µs |
| the whole host resumed refill, 12 bricks, 200 items | 18 µs |
| the whole host resumed refill, 12 bricks, 20,000 items | 155 µs |
| the copy residency would save, 12 bricks | 24 KiB, 0.9 µs |

A device seeded kernel still has to dispatch, and the dispatch alone — of the
emptiest tape that exists, before any seed buffer, seed eviction or device-side
`had_acc` reduction — already costs more per brick than the entire host-side
resumed refill, **including the per-brick suffix compile a device path would
have to pay as well**. The crossover would need a window of hundreds of
resumable bricks, and a window that large is one the stroke has not covered
before, so its bricks have no seed to be resident: measured at 256 bricks, 6% of
the window could be resumed at all.

`design.md` carries the numbers and the method.

## Non-goals

**Metal.** `clay_device_adopt` serves Metal and Vulkan; Metal has no seeded path
here and falls back to the full walk, correctly and silently. Filed as a
device-gated follow-up (#350) — it needs an Apple GPU to validate and this work
was done on Linux.

**CUDA and OpenCL are not reachable from this entry point at all.** Neither has
a device-adoption path (`eval::make_backend` serves Vulkan and Metal; OpenCL is
not in `DeviceApi`), so a caller cannot obtain a `clay_device` for them and no
seeded kernel written for them could be invoked here. Giving them one is device
interop, not this.

**Brick storage stays host-side.** This makes evaluation output device-resident,
not the cache. Generations, staleness, classification, quantization and the
memory budget are unchanged.

**The C ABI is unchanged.** No new entry point, no struct, no enumerator; the
device refill computes the same values it always did and reports through the
`clay_document_resume_stats` counters #343 added. No version bump.
