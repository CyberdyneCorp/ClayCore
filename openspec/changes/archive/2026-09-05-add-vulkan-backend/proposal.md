# Proposal: a Vulkan compute backend, via the dialect that already compiles

## Why

The backend interface was written with this in mind. `evaluation-backends` says
of OpenCL that "the backend interface SHALL NOT assume OpenCL specifics, so a
future Vulkan-compute backend can slot into the same interface", and
`docs/05-claycore-library.md` calls OpenCL "tier-3, best-effort" with "Vulkan
compute … the likely long-term replacement". The slot exists and is empty.

The case for filling it now is that OpenCL is not carrying the tier it was given:

- Its CI job was **removed** (2026-08-07) because running parity against pocl
  compared the CPU's arithmetic with itself. Registering and passing parity on a
  real device is now a manual, hardware-dependent release check.
- It reports `Unsupported` for raycast, because the sphere-tracing utilities are
  templated C++ that OpenCL C cannot compile, and for device meshing.
- Vendor support is uneven and, on the platforms where a GPU backend would
  matter most outside Apple, declining.

Vulkan is the portable compute API that is actually present: on Linux and
Windows GPUs of every vendor including those without CUDA, on software runtimes
for CI, and — relevant later rather than now — on hardware this proposal is not
about.

**This does not speed up the iPad.** Vulkan on Apple hardware means MoltenVK
translating to Metal, which cannot beat the Metal backend it would be
translating into. That is worth stating plainly because this change is being
proposed alongside iPad latency work and is not part of it: it is portability
and the retirement path for tier 3.

## What changes

A Vulkan compute backend registered like the others, implementing at minimum
`eval_points` and the grid evaluation that fills bricks, reporting `Unsupported`
for what it does not provide, and passing the parity suite where registered. Its
absence on any platform blocks nothing, exactly as OpenCL's does not.

## The shader route — and why it is probably not a new dialect

The kernels are single-source and already compile under four profiles (CPU,
CUDA, Metal, and the OpenCL amalgamation), gated on every push by
`check_kernel_dialect.py`. A fifth hand-written dialect is the expensive answer
and the one most likely to drift.

**The OpenCL amalgamation already exists and is already the C-compatible subset
of the kernels.** `clspv` compiles OpenCL C to Vulkan SPIR-V; it is built for
precisely this migration. So the likely route is: existing amalgamation →
clspv → SPIR-V → Vulkan compute, with no new dialect, no second copy of the
maths, and the dialect check already covering the source.

The alternatives — a GLSL port, or Slang/HLSL as a new front end — are real
options and both mean a fifth profile to keep in step. They should be measured
against the clspv route rather than assumed worse, and the decision recorded.

## What it is not

**Not an iPad change.** Stated again because it will be read next to changes
that are.

**Not the removal of OpenCL.** If this lands and holds its tier, retiring OpenCL
becomes a proposal with an argument; making that decision inside this one would
be deciding it without evidence.

**Not a raycast implementation.** Sphere tracing is templated C++ and Vulkan
compute will hit the same wall OpenCL did. `Unsupported` is the honest answer
and the interface already handles it.

**Not a CI parity gate that proves arithmetic.** A software Vulkan runtime
(lavapipe, SwiftShader) executes on the CPU, so parity against it is the pocl
trap in a new costume — it gates the *plumbing* (SPIR-V validity, descriptors,
dispatch, buffer layout) and not the numbers. That distinction must be written
into the job's name and its documentation, or the job will be believed.

## Open questions

- **clspv, GLSL, or Slang.** To be decided in `design.md`, on a real compile of
  the current kernel set rather than on reputation. clspv's support for the C
  subset the amalgamation uses is the thing to verify first — the shim macros
  already constrain that subset, which is what makes this plausible.
- **What `eval_bricks` costs here.** The Metal measurement says an 8³ brick is
  too little work to cover a dispatch. Vulkan's dispatch cost is not Metal's and
  the crossover has to be found, not inherited.
- **Descriptor and buffer strategy.** The Metal path's per-call allocation and
  re-upload is a known cost being fixed separately; this backend should be built
  with tape residency and buffer reuse from the start rather than repeating it.
- **Which Vulkan version and which extensions.** The floor should be what is
  actually present on target hardware, stated once, and not raised casually.
- **Whether fp16 storage is claimed.** `BackendCaps` has the flag;
  `VK_KHR_shader_float16_int8` availability decides it.

## Impact

`evaluation-backends` gains the backend and its tier. `build-packaging` gains a
preset, the dialect-check profile for whatever shader route is chosen, and the
honest description of what a software-runtime CI job does and does not prove.
`sdf-kernels` gains a statement only if a new profile is chosen — under the
clspv route it gains nothing, which is the argument for that route.
