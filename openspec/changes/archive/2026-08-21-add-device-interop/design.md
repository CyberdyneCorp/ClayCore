# Design: add-device-interop

## 1. A device crosses as opaque handles under a named API

`clay.h` must stay free of vendor headers — `FFI-general design` requires it and
every bindings generator depends on it. So a device is described, not typed:

```c
typedef enum clay_device_api {
    CLAY_DEVICE_API_METAL = 0,
    CLAY_DEVICE_API_VULKAN = 1,
    CLAY_DEVICE_API_CUDA = 2
} clay_device_api;

typedef struct clay_device_desc {
    uint32_t struct_size;
    int32_t api;            /* clay_device_api */
    void* handles[6];       /* API-specific, positions documented below */
    uint32_t queue_family;  /* Vulkan only; ignored elsewhere */
} clay_device_desc;

typedef struct clay_device clay_device;  /* opaque */

clay_device* clay_device_adopt(const clay_device_desc* desc);
void clay_device_release(clay_device* device);
```

`handles` positions, documented in the header per API and validated on adopt:

| API | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| Metal | `id<MTLDevice>` | `id<MTLCommandQueue>` | — | — |
| Vulkan | `VkInstance` | `VkPhysicalDevice` | `VkDevice` | `VkQueue` |
| CUDA | `CUcontext` | `CUstream` | — | — |

A fixed `void*[6]` rather than a union, because a union of vendor types is a
vendor header and a union of `void*` is this with worse ergonomics. An API whose
backend is not compiled in is refused at `adopt`, not at first use.

**Rejected: a device per backend-specific entry point** (`clay_metal_adopt`,
`clay_vulkan_adopt`). It is more type-safe in C and it puts platform symbols in
the ABI, so the set of functions `clay.h` declares changes with the build
configuration — which `tools/check_c_abi.py` checks against the built library
and which would make the header configuration-dependent for every generator.

## 2. A backend instance is bound to a device; the registry is not

Today `eval::Registry` maps a name to one process-wide backend. A supplied
device cannot go there: two hosts, two devices, one name.

So the registry keeps handing out the device-owning backends it hands out today,
and a device-bound backend is a **separate instance** the caller holds:

```cpp
namespace clay::eval {
class Device;                                  // opaque, API-specific impl
std::unique_ptr<Backend> make_backend(std::string_view name, Device& device);
}
```

At the ABI a `clay_device` carries the instance, and the calls that take a
`const char* backend` gain a device-taking sibling rather than an overloaded
string. Naming a device through the backend string ("metal@0x7f…") was
considered and rejected on sight.

**Backends that cannot adopt report `Unsupported` from `make_backend`** — the
established answer for a capability a backend does not provide, and it keeps
"availability changes speed, never results" true: a host whose adopt fails falls
back to the registry's own backend and gets identical values.

## 3. claycore records work; the host owns submission

The rule that keeps this from becoming a source of driver crashes:

- **claycore SHALL NOT create, destroy or wait on synchronization primitives the
  host owns**, and SHALL NOT submit to a supplied queue outside a call the host
  made.
- A device-bound call SHALL either complete its work before returning, or return
  a completion token the host waits on. It SHALL NOT leave work in flight with
  no way to know when it lands.
- Thread-safety follows the brick cache's existing rule rather than inventing a
  second one: **calls on one `clay_device` are the host's to serialize.** A GPU
  queue is not free-threaded and pretending otherwise here would be a threading
  policy the consumer did not ask for.

The first release completes before returning. It is the simple contract, it is
what a host driving a preview at edit rate needs, and a token can be added later
behind the `struct_size` rule; a token that is wrong is worse than a wait.

## 4. Device-resident evaluation output

The point of the whole change:

```c
typedef struct clay_device_buffer {
    uint32_t struct_size;
    void* handle;     /* MTLBuffer / VkBuffer / CUdeviceptr, per the device's API */
    uint64_t offset;  /* bytes */
    uint64_t size;    /* bytes available from offset */
} clay_device_buffer;

clay_result clay_eval_grid_device(const clay_document* doc, clay_device* device,
                                  const clay_grid_query* grid,
                                  const float region_min[3], const float region_max[3],
                                  const clay_device_buffer* out_values,
                                  const clay_device_buffer* out_colors_rgb);
```

Same semantics as `clay_eval_grid` — same cull region, same x-fastest order,
same `float` element type — differing only in where the results land. It is
deliberately the same call with a different destination so that a host can
A/B the two and get bit-identical values, which is also the test.

`size` is required and checked against the lattice, keeping the "a count is
never inferred" rule this ABI holds everywhere. A buffer too small is refused
rather than partially written.

Bricks reach the same path through the existing `clay_brick_cache_eval_requests`
shape: a device-taking sibling writing brick `i` at `i * dim³` floats into one
device buffer. That is the call ClaySpaceDesktop actually wants, and it needs no
new semantics once the grid form exists.

**Values stay `float` on the device.** Not fp16, even though the cache stores
fp16 and a host uploading an `r16float` atlas wants fp16: quantization is
`BrickCache::submit`'s job and it also classifies and band-clamps. A device path
that quantized would be a second implementation of the cache's most
correctness-sensitive step, and the two would drift. A host taking the device
path owns the conversion, and `close-webgpu-host-abi-gaps` is the change that
gives it the host-memory route where the cache does it correctly.

## 5. What proves it works

Parity, not benchmarks. A device-bound backend that produces different values
from the registry's own is the failure mode that matters, and it is cheap to
gate: run the existing parity suite twice, once through each, and require
agreement at the tolerance `GPU parity tolerance` already sets.

For the device-buffer path specifically: evaluate a grid to host memory and to a
device buffer, map the device buffer, and require **bit-identical** floats — not
a tolerance. It is the same kernel on the same device; anything but equality is
a bug in the plumbing, which is precisely what this test is for.

CI runs it on Vulkan against lavapipe, adopting a device the test creates —
which is a real adopt of a device claycore did not make, even though the test
made it. Metal adoption is a manual hardware check alongside the others in
`docs/RELEASE.md`.
