# Tasks: add-device-interop

## 1. The device abstraction

- [ ] 1.1 `eval::Device` — an opaque, API-tagged holder of borrowed native handles; it retains nothing it did not create and destroys nothing it did not make
- [ ] 1.2 `eval::make_backend(name, Device&)` returning an instance the caller owns, or nullptr with `Unsupported` when the backend has no adoption path; the process-wide `Registry` is untouched
- [ ] 1.3 State and enforce the ownership rule in the interface header: no synchronization primitive of the caller's is created, destroyed or waited on, and work issued during a call is complete when the call returns

## 2. Adoption per backend

- [ ] 2.1 Vulkan: adopt `VkInstance` / `VkPhysicalDevice` / `VkDevice` / `VkQueue` + queue family; validate the family supports compute and that the device is the one the buffers come from
- [ ] 2.2 Metal: adopt `MTLDevice` + `MTLCommandQueue`; the existing per-call allocation becomes an allocation on the adopted device
- [ ] 2.3 CUDA: adopt `CUcontext` + `CUstream`, or report `Unsupported` and say so in the header table rather than leaving it ambiguous
- [ ] 2.4 CPU and OpenCL report `Unsupported`; a caller falling back gets identical values

## 3. Device-resident evaluation

- [ ] 3.1 `Backend::eval_grid_device(tape, query, DeviceBuffer values, DeviceBuffer colors)` defaulting to `Unsupported`, so no backend is forced to implement it
- [ ] 3.2 Vulkan implementation: dispatch writing into the caller's `VkBuffer` at the given offset
- [ ] 3.3 Metal implementation: dispatch writing into the caller's `MTLBuffer` at the given offset
- [ ] 3.4 Size checking against the lattice before any dispatch; an undersized buffer is refused with nothing written

## 4. The ABI

- [ ] 4.1 `clay_device_api`, `clay_device_desc` (with the per-API handle table in the header), `clay_device_adopt`, `clay_device_release`
- [ ] 4.2 `clay_device_buffer`, `clay_eval_grid_device`, `clay_brick_cache_eval_requests_device`
- [ ] 4.3 `clay_device_backend_name` / capability query, so a host can tell what it adopted and what that backend supports
- [ ] 4.4 `tools/check_c_abi.py` passes with no vendor header reachable from `clay.h`, and the declared surface does not vary with build configuration — add the check if the existing one does not cover it
- [ ] 4.5 `tools/check_binding_parity.py`: device adoption is exempt with its reason (a Python consumer has no device to lend), and the exemption states what would change that

## 5. Tests

- [ ] 5.1 Parity suite run through an adopted device and through the registered backend; agreement at the existing GPU tolerance, and identical capability reports
- [ ] 5.2 Grid evaluated to host memory and to a device buffer on the same device: **bit-identical** floats, not a tolerance
- [ ] 5.3 Bricks evaluated into one device buffer at fixed stride; each brick matches its host-path values bit for bit
- [ ] 5.4 Adoption refused: an unbuilt API, an incomplete handle set, a queue family without compute — each refused at adopt with nothing retained
- [ ] 5.5 Undersized device buffer refused with nothing written
- [ ] 5.6 The library holds no caller-owned object after a device-bound call returns — checked with a validation layer where one exists
- [ ] 5.7 CI: run 5.1–5.5 on Vulkan against lavapipe, adopting a device the test creates. This gates the plumbing, not the arithmetic — the same distinction `add-vulkan-backend` had to make explicit after the OpenCL/pocl job

## 6. Documentation

- [ ] 6.1 `docs/05-claycore-library.md`: the device-ownership contract and the queue/thread rule
- [ ] 6.2 `docs/06-host-gpu-previews.md`: the three routes a host now has — compile our kernels (tape export), upload the brick atlas (`close-webgpu-host-abi-gaps`), or lend us the device — and which to pick
- [ ] 6.3 Say plainly, in the header and in the docs, that the brick **cache** is host-resident and that this change makes evaluation output device-resident, not brick storage
- [ ] 6.4 `openspec/ROADMAP.md`, and reply on issue #43 item 6 with what was decided against (exported allocations) and why
