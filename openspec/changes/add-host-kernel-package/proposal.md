# Proposal: ship the kernel headers to host GPU previews

## Why

Design principle 1 says every distance function and operator is written once
and compiled into every backend, "no Swift/MSL/CUDA copies drifting apart".
That holds inside this repository — CPU, Metal, CUDA and OpenCL all compile
`include/clay/kernel/*.h` — and fails immediately outside it, because nothing
we publish contains those headers. The xcframework ships `clay.h` and a static
library; a host that wants to *preview* a document on the GPU before ClayCore
bakes it has no choice but to write the math again.

ClaySpace did exactly that and it drifted. Its Metal preview used a mix-form
quadratic smin of support `k`; `csmin_quadratic` uses support `4k`. Every
blend in the real document field was four times wider than the preview drew,
so sculpts looked crisp until the document was baked through `clay_eval_points`
— at which point carved faces flattened and separate limbs fused, and the bake
was blamed for destroying strokes it had merely revealed. The app fixed it by
hand-mirroring four functions line for line, which is the same copy again: the
next blend profile, extended op or deformer re-opens the bug.

Two things are missing, and neither is math. The headers are not *shipped*, and
there is no fixture a host can run to find out that its preview disagrees.

## What Changes

- **The kernel headers self-select their backend.** `shim.h` today defaults to
  the CPU branch unless the build defines `CLAY_KERNEL_METAL`. It SHALL first
  consult the compiler's own identity — `__METAL_VERSION__`, `__CUDACC__`,
  `__OPENCL_VERSION__` — so a host that drops the headers into a `.metal` file
  and includes them gets MSL with no build settings at all. An explicit
  `CLAY_KERNEL_*` still wins.
- **One umbrella header**, `clay/kernel/kernels.h`, pulling in the whole
  dialect in dependency order, so a host preview shader has a single include
  rather than a list it has to keep in step with ours.
- **A published kernels artifact.** `tools/package_kernels.py` emits
  `dist/claycore-kernels/`: the headers verbatim, the parity fixture, and a
  README with the three-line MSL usage. `tools/build_xcframework.sh` copies the
  same headers into the xcframework's `Headers/clay/kernel/`, so a SwiftPM
  consumer already has them on disk.
- **A parity fixture hosts can run.** ClayCore exports composed tapes, probe
  points and the CPU-reference distance and color at each probe as JSON
  (`clay parity-fixture -o …`). A host evaluates the same tapes with kernels
  compiled from these headers and asserts agreement, the way
  `tests/unit/test_parity.cpp` does across our own backends. The cases cover
  exactly the surface that drifts: every blend profile against every smooth
  boolean, every extended mode, the `h²` color weights, the deformer chain,
  and one composed document.
- **Metal joins the dialect gate.** `tools/check_kernel_dialect.py` compiles
  every kernel header as MSL with `xcrun metal` where one exists, and against a
  stubbed `metal_stdlib` everywhere else — the same host-emulation trick the
  CUDA profile already uses. A missing overload in the Metal branch of the
  shim currently only surfaces on an Apple machine; after this it fails on
  Linux CI too.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: backend self-selection, the umbrella header, and the promise
  that the dialect headers are consumable by a host shader compiler.
- `build-packaging`: the kernels artifact, the parity fixture it carries, and
  the Metal profile in the dialect gate.

## Impact

- `include/clay/kernel/shim.h` (detection block), new
  `include/clay/kernel/kernels.h`.
- New `include/clay/io/parity_fixture.h` + `src/io/parity_fixture.cpp`, a
  `clay parity-fixture` subcommand, `tests/unit/test_parity_fixture.cpp`.
- `tools/package_kernels.py`, `tools/check_kernel_dialect.py`,
  `tools/build_xcframework.sh`, `.github/workflows/ci.yml`.
- `docs/06-host-gpu-previews.md`, `README.md`, `openspec/ROADMAP.md`.
- No ABI change: nothing here alters an existing symbol or struct, and the new
  fixture export is additive and CLI-only for now.

## Out of scope

Feeding a live `clay_document`'s tape to a host GPU still needs the tape
buffers across the C ABI — the third item in the issue, deliberately left for
its own change. What lands here is what makes that change small: the host-side
evaluator it would feed is `ctape_eval`, compiled from these headers, and the
fixture proves it agrees before any live document is involved.
