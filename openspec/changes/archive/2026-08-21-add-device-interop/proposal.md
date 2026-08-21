# Proposal: a backend owns its device, so nothing can be shared with the host's

## Why

Issue #43 item 6, from ClaySpaceDesktop:

> There is no `device`, `context`, `queue`, `shared` or `external` anywhere in
> `clay.h`. Backends create and own their devices. […] On macOS both claycore
> and `wgpu` are on Metal, and on Linux both would be on Vulkan — in both cases
> the same buffer could serve both sides, but the ABI gives no way to express
> it, so every brick and every mesh crosses host memory.

The observation is exactly right and the consequence is structural rather than
incidental. `eval::Backend` has no notion of a device at all: `eval_grid` takes
a tape and a lattice and writes `float*` — host memory, by type. Every backend
therefore allocates its own device, dispatches, and reads back, and a host that
was going to draw on a GPU pays an upload it should never have needed:

```
claycore GPU  ->  device memory  ->  HOST memory  ->  host's device memory  ->  draw
                                     ^^^^^^^^^^^
                                     the round trip this change removes
```

With `add-vulkan-backend` landed this is now the same physical device on both
supported platforms — Metal on macOS, Vulkan on Linux — which is what makes the
gap worth closing rather than merely worth noting. The two changes pair, as the
issue says.

## What changes

**A device is something the caller can supply.** An opaque `clay_device`
constructed from the host's own native objects, and backend instances bound to
it, so claycore dispatches onto the host's device and queue rather than one it
made.

**Evaluation can write to device memory.** A grid evaluation whose destination
is a caller-owned native buffer at a caller-chosen offset, so the values a host
was going to upload are produced where it was going to upload them to.

Nothing about `clay.h`'s freedom from vendor headers changes: native objects
cross as `void*` under a named API, never as `VkDevice` or `id<MTLDevice>`. A
header that included `vulkan.h` would break `FFI-general design` and every
bindings generator that reads it.

## What this change does not do

**It does not make the brick cache device-resident,** and that limit is the most
important sentence in this proposal. The cache's state machine — generations,
staleness, band classification, fp16 quantization, the memory budget — is host
code over host memory, and it is where a submitted brick becomes a stored brick.
So the zero-copy this change delivers is on **evaluation output**, not on cache
storage: a host can have its bricks computed straight into its own buffer, and
then owns quantizing and uploading them itself, at which point the cache is not
in the loop and neither are its guarantees.

That is a real and useful thing — it is the whole preview path — and it is not
the same thing as "the brick cache lives on the GPU". Anyone who wants the
latter should read this proposal as explicitly not providing it.

**It does not export claycore-allocated memory to the host.** Given device
injection, exporting is the strictly worse half of the issue's "either/or":
sharing an allocation across two devices needs external-memory extensions,
matching physical devices, and a per-API handle lifetime (an fd on Linux, a
`MTLBuffer` reference on macOS) that the ABI would then own. Sharing a *device*
makes all of that unnecessary — one device, one allocator, no extension, no
handle to leak. If a host genuinely cannot yield its device, that is the case
for exporting, and it should be argued from a host that has it.

**It does not touch CPU behaviour.** A device-less backend and a device-less
call keep working exactly as they do; this is additive on both sides.

**It does not promise device meshing.** `BackendCaps::device_meshing` exists and
is false everywhere; a mesh crossing as device memory is a separate ask that
needs a device mesher first.

## Risk, stated up front

This is the largest item in issue #43 and the one most able to introduce a
class of bug the rest of the codebase does not have: a crash inside a host's
GPU driver, from claycore submitting to a queue whose synchronization the host
also uses. The design's answer is that claycore SHALL NOT own submission
ordering on a supplied queue — it records work, and the host decides when it
runs — but the honest summary is that this trades an unnecessary copy for a
contract about queue and thread ownership that both sides have to keep.

Sequenced last of the four issue-#43 changes for that reason.
