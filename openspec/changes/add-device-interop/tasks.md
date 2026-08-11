# Tasks: add-device-interop

## 1. The device abstraction

- [x] 1.1 `eval::Device` — an opaque, API-tagged holder of borrowed native handles; it retains nothing it did not create and destroys nothing it did not make
- [x] 1.2 `eval::make_backend(name, Device&)` returning an instance the caller owns, or nullptr with `Unsupported` when the backend has no adoption path; the process-wide `Registry` is untouched
- [x] 1.3 State and enforce the ownership rule in the interface header: no synchronization primitive of the caller's is created, destroyed or waited on, and work issued during a call is complete when the call returns

## 2. Adoption per backend

- [x] 2.1 Vulkan: adopt `VkInstance` / `VkPhysicalDevice` / `VkDevice` / `VkQueue` + queue family; validate the family supports compute and that the device is the one the buffers come from
- [x] 2.2 Metal: adopt `MTLDevice` + `MTLCommandQueue`. **Written but NOT compiled or run here** — this machine is Linux and has no metal-cpp toolchain. It mirrors the Vulkan change structurally (split `init()` into device acquisition + `build_pipelines()`, an `owns_device_` flag the destructor honours, per-buffer offsets on the encoder). The macOS CI job compiles it on every push and the Metal parity job runs it, so a break surfaces in CI rather than at a consumer — but it must be exercised on real hardware before a release, and `docs/RELEASE.md` carries that
- [x] 2.3 CUDA reports "cannot adopt" and the header table says so IN CAPITALS beside the enumerator, rather than leaving a reader to discover it at runtime. The enumerator exists so the layout does not shift when an implementation lands
- [x] 2.4 CPU and OpenCL report `Unsupported`; a caller falling back gets identical values

## 3. Device-resident evaluation

- [x] 3.1 `Backend::eval_grid_device(tape, query, DeviceBuffer values, DeviceBuffer colors)` defaulting to `Unsupported`, so no backend is forced to implement it
- [x] 3.2 Vulkan implementation: dispatch writing into the caller's `VkBuffer` at the given offset — descriptor bindings 3 and 4 are overridden with the caller's slice, everything else binds as before, so the two paths run the same shader over the same tape
- [x] 3.3 Metal implementation: dispatch writing into the caller's `MTLBuffer` at the given offset
- [x] 3.4 Size checking against the lattice before any dispatch, on both backends and at the ABI. For the brick form the WHOLE batch's stride is checked up front, so a buffer that cannot hold every brick is refused before the first dispatch rather than after the ones that fit have landed — tested by comparing the buffer's contents across a refused call

## 4. The ABI

- [x] 4.1 `clay_device_api`, `clay_device_desc` (with the per-API handle table in the header), `clay_device_adopt`, `clay_device_release`
- [x] 4.2 `clay_device_buffer`, `clay_eval_grid_device`, `clay_brick_cache_eval_requests_device`
- [x] 4.3 `clay_device_backend_name` / capability query, so a host can tell what it adopted and what that backend supports
- [x] 4.4 `tools/check_c_abi.py` passes with no vendor header reachable from `clay.h`, and the declared surface does not vary with build configuration — add the check if the existing one does not cover it
- [x] 4.5 `tools/check_binding_parity.py`: device adoption is exempt with its reason (a Python consumer has no device to lend), and the exemption states what would change that

## 5. Tests

- [x] 5.1 Parity suite run through an adopted device and through the registered backend; agreement at the existing GPU tolerance, and identical capability reports
- [x] 5.2 Grid evaluated to host memory and to a device buffer on the same device: **bit-identical** floats, not a tolerance — verified on lavapipe, including an offset into a larger buffer and that nothing before the offset is touched
- [x] 5.3 A whole drain evaluated into one device buffer at fixed stride through `clay_brick_cache_eval_requests_device`; every brick matches the host-memory path, and an undersized batch buffer is refused with the buffer's contents unchanged
- [x] 5.4 Adoption refused: an unbuilt API, an incomplete handle set, a queue family without compute — each refused at adopt with nothing retained
- [x] 5.5 Undersized device buffer refused with nothing written
- [ ] 5.6 **NOT DONE.** The ownership rule is enforced structurally (an `owns_device_` flag the destructor honours) and is stated in the interface header, but nothing asserts it. It wants a run under `vulkan-validationlayers` with object-lifetime checks on; the CI job installs the package but does not enable the layer. Worth doing before anyone ships on this
- [x] 5.7 Covered by the `vulkan-plumbing` job added with `add-vulkan-backend`: it runs `ctest --preset vulkan`, which includes these cases, and its name and documentation already carry the plumbing-not-arithmetic distinction

## 6. Documentation

- [x] 6.1 `docs/05-claycore-library.md`: the device-ownership contract and the queue/thread rule
- [x] 6.2 `docs/06-host-gpu-previews.md`: the three routes a host now has — compile our kernels (tape export), upload the brick atlas (`close-webgpu-host-abi-gaps`), or lend us the device — and which to pick
- [x] 6.3 Say plainly, in the header and in the docs, that the brick **cache** is host-resident and that this change makes evaluation output device-resident, not brick storage
- [x] 6.4 `openspec/ROADMAP.md`; the issue reply covers item 6 with what was decided against (exported allocations) and why
