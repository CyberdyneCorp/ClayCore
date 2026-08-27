# Design: which shape the device refill resumes in

The issue (#345) names two shapes and asks for a measurement before either is
designed. This is the measurement.

## The two shapes

**(a) Device-resident seeds.** A per-brick GPU buffer beside the atlas. The
suffix is compiled on the host, uploaded, and evaluated by a seeded kernel that
reads the seed buffer and writes both the caller's slot and the seed buffer.
Nothing crosses the device boundary.

**(b) Host-side resume, written across.** A brick that can be resumed is
answered by the host code that already exists and written into its slot; a brick
that cannot is evaluated on the device and read back to become the next seed.
Two transfers of a few kilobytes a brick.

## What decides between them

Both shapes pay the **per-brick suffix compile on the host**: `plan_resume`
walks the append log, `compile_layer_suffix` culls the appended items against
that brick's own region, and neither is something a kernel can do. So the
comparison is not "GPU against CPU"; it is

    host seeded evaluation + a copy      against      one device dispatch

with everything else common. Residency's whole saving is the copy.

## Method

A probe driving the real C ABI against a Vulkan device created the way a host
would, on this repository at `26750c6` (before the change) and after it. The
sculpt is `abi_sculpt` from `benchmarks/bench_main.cpp` — a sphere plus `n`
scattered dabs — and the window is 12 bricks of a dim-8, 0.05-voxel cache. Each
row is 40 dabs, one appended item per dab, timed individually and averaged.

The machine is shared, so what is quoted is ratios and the load average is taken
on both sides of every run (2.98/6.94/5.26 before, 6.08/7.70/7.66 after; the
timings below moved by less than the spread between repeats).

The **dispatch floor** row is the same 12-brick device refill against a
ONE-ITEM document. It is the least a device seeded path could possibly cost:
the emptiest tape that exists, no seed buffer, no seed eviction, no device-side
`had_acc` reduction, just the submissions.

## Numbers (RTX 5060, driver 580, Vulkan 1.3)

| history | bricks | device full walk (before) | host resumed refill | dispatch floor | H2D copy |
|--------:|-------:|--------------------------:|--------------------:|---------------:|---------:|
|     200 |     12 |                         — |             0.018 ms |       0.283 ms | 0.0009 ms |
|   1,000 |     12 |                         — |             0.032 ms |       0.335 ms | 0.0009 ms |
|   5,000 |     12 |                  15.229 ms |             0.049 ms |       0.283 ms | 0.0008 ms |
|  20,000 |     12 |                  59.252 ms |             0.210 ms |       0.277 ms | 0.0009 ms |
|  20,000 |     64 |                  66.830 ms |             0.428 ms |       2.144 ms | 0.0055 ms |
|  20,000 |    256 |                  82.529 ms |             0.641 ms |       7.368 ms | 0.0495 ms |

Dispatch floor against window size, on a one-item document:

| bricks | 1 | 2 | 4 | 12 | 32 |
|-------:|--:|--:|--:|---:|---:|
| ms | 0.023 | 0.067 | 0.101 | 0.285 | 0.744 |

That is **23 µs a brick**, near-linear, because `eval_grid_batch_device`'s
default loops the per-grid device path and the Vulkan backend does not override
it: one command buffer, one submit and one fence wait per 8³ lattice.

## What the numbers say

**Residency cannot win on the windows a sculpt submits.** At 12 bricks the
dispatch floor is 0.277–0.335 ms whatever the document holds, while the whole
host-side resumed refill — the shared suffix compile included — is 0.018 ms at
200 items and 0.210 ms at 20,000. The copy residency exists to remove is 24 KiB
and 0.9 µs, three orders of magnitude below the dispatch it would replace it
with.

**The crossover, stated honestly.** Even granting a backend that batched the
whole window into ONE dispatch — which none here does — the floor would be
~23 µs plus the per-brick suffix tape uploads, against ~1.5 µs a brick for the
host seeded evaluation at 200 items. That crosses at roughly 15 resumable
bricks *for the evaluation alone*, and it is the wrong 15: at 64 bricks only 24%
of the window could be resumed at all, and at 256 bricks 6%. A window large
enough for residency to pay is a window the stroke has not covered, whose bricks
have no seed to be resident. As the per-grid loop actually stands the crossover
does not exist at any size.

**Shape (b), landed.** The 12-brick, 20,000-item dab goes 59.25 ms → 0.27 ms,
which is 1.35x the pure host resumed refill; the remainder is the single copy
dispatch that carries the answers across. At 5,000 items, 15.23 ms → 0.08 ms.

## The copy primitive

The transfer cannot be `vkCmdCopyBuffer`. The caller's buffer was created for
its own renderer, and nothing obliges it to carry
`VK_BUFFER_USAGE_TRANSFER_SRC` or `_DST` — the test's own host buffers do not.
A transfer command on a buffer created without them is undefined behaviour in
the caller's process. The evaluation path already binds that buffer as a
**storage** buffer, so a copy expressed as storage reads and writes needs no
usage the caller did not already have to supply. One extra entry point in
`clay_kernels.comp.in`, one extra pipeline, and binding 2 gains the caller
override that bindings 3 and 4 already had; the read direction is the write
direction with the two ends exchanged.

## Why multi-layer documents are excluded

A seed is two values when layers sit beneath the active one, and this path
compiles the document WHOLE — `ChunkHalf::Whole` — so what it could read back is
neither half. Storing it as the active half would be a lie the shape gate does
not catch forever: `shaped_entry` refuses a below-less entry to a reader that
wants one, but a document that later loses a layer stops wanting one, and the
stale whole-document value would then pass. Storing nothing is the only answer
that stays true. The host-memory form runs two batches and keeps the halves
apart; matching that here means two device batches and a device-side fold, which
is a change of a different size.

## What a follow-up would need

A device seeded kernel is still the right shape for a **large** dirty window —
a re-bake, a resolution change, a scene load — where the resumable fraction is
high because the ground has been covered before. Reaching that regime needs
`eval_grid_batch_device` overridden to one dispatch per chunk first, which is
`speed-the-metal-path`'s concatenated-tape trick applied to Vulkan and is a
prerequisite, not a detail: without it the per-brick dispatch dominates
everything either shape could save.
