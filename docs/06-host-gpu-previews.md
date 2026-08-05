# Host GPU previews — using claycore's kernels instead of copying them

A host that draws a live preview of a document while claycore bakes, meshes or
exports it has to evaluate the field twice: once on its GPU, once in the
engine. Design principle 1 ([05](05-claycore-library.md#2-design-principles))
says there is **one** implementation of every distance function and operator —
this document is how that promise reaches code outside this repository.

## Why this exists

ClaySpace's Metal preview re-implemented the kernel math by hand. It used a
mix-form quadratic smin of support `k`; `kernel/ops.h` uses support `4k`:

```c
CLAY_FN float csmin_quadratic_support(float k) { return 4.0f * k; }
```

Every blend in the real document field was therefore four times wider than the
preview drew. Sculpts looked crisp and strokes looked analytic right up to the
moment the document went through `clay_eval_points`, at which point carved
faces flattened and separate limbs fused. The bake was blamed for destroying
strokes it had only revealed. Fixing it meant hand-mirroring four functions
line for line — which is the same copy again, and would drift again at the next
blend profile, extended op or deformer.

The fix is not more careful copying. It is not copying.

## Use the headers

`include/clay/kernel/` is header-only, allocation-free and written in a
restricted dialect every target compiler accepts. Include the umbrella:

```metal
#include "clay/kernel/kernels.h"
using namespace clay::kernels;   // MSL reserves `kernel`; see shim.h
```

No build settings are needed. `shim.h` reads the compiler's own identity —
`__METAL_VERSION__`, `__CUDACC__`, `__OPENCL_VERSION__` — and selects the
matching branch; define `CLAY_KERNEL_METAL` (or `_CUDA` / `_OPENCL` / `_CPU`)
only to override it.

Where the headers come from:

| Source | For |
|---|---|
| `dist/claycore-kernels/` (`python3 tools/package_kernels.py`) | any host; carries the headers, the parity fixture and a worked MSL example |
| `claycore.xcframework/<slice>/Headers/clay/kernel/` | a SwiftPM/Xcode app that already links the framework |
| the repository's `include/` | building against a source checkout |

All three are the same bytes. The packaging script copies; it never
transforms, because a generated second version of the math is the failure
being prevented.

`dist/claycore-kernels/examples/host_preview.metal` is a complete evaluator
over claycore tape buffers — `ctape_eval` for distance and color, `cnormal`
for shading — and contains no math of its own.

## Prove the preview agrees

Shared headers stop the math drifting. They do not stop a host from feeding
them the wrong parameters, traversing a tape wrongly, or reading the
out-of-line blob at the wrong offset. The parity fixture is the gate for that:

```sh
clay parity-fixture -o kernel_parity.json
```

It is JSON, schema 1:

```json
{
  "schema": 1,
  "generator": "claycore 0.14.0",
  "tolerance": {"distance_abs": 1e-05, "distance_rel": 0.0001, "color_abs": 0.0001},
  "cases": [
    {
      "name": "blend_union_quadratic",
      "note": "two spheres across the seam plane; ...",
      "tape": {"instrs": [[op, param_offset], ...], "params": [...], "blob": [...],
               "is_exact": true, "safe_step_scale": 1.0},
      "points": [[x, y, z], ...],
      "distance": [...],
      "color": [[r, g, b], ...]
    }
  ]
}
```

For each case, upload `instrs` / `params` / `blob` as the three tape buffers,
evaluate at every point in `points`, and assert

```
|got - expected| <= distance_abs + distance_rel * |expected|
```

That is the same comparison `tests/unit/test_parity.cpp` applies across
claycore's own backends, at the same tolerances — a host GPU is no more
bit-exact than ours.

The 32 cases are chosen for what a hand-written preview gets wrong rather than
for coverage of the primitive set: every blend profile against smooth union,
subtraction and intersection; all nine extended combine modes; the material-mix
weights of a colored blend; a deformer chain and a region deformer; finite
repetition; a revolved polygon and a stroke chain (both of which carry data in
the blob); and one composed multi-layer document with a blended mirror, a
nested group and a paint pass.

The blend cases probe a ring straddling the seam of two overlapping spheres,
which is exactly where a wrong support width shows up. `tests/unit/test_parity_fixture.cpp`
asserts that a support-`k` quadratic smin — the original drift — fails the
fixture, so the gate cannot quietly stop discriminating.

Floats are written in the shortest form that round-trips, so a consumer reading
them back gets the bits claycore evaluated. Export is deterministic: two runs
of one build produce identical files, and a diff means the engine changed.

## What is not here yet

Feeding a **live** document's tape to a host GPU still needs the tape buffers
across the C ABI; today `scene::Tape` is C++-side only, so a host gets tapes
from the fixture but not from `clay_document`. That is its own change, and a
small one now: the evaluator it would feed is `ctape_eval` compiled from these
headers, already proven against the fixture.

## Keeping this honest

- `tools/check_kernel_dialect.py` compiles every kernel header as MSL — with
  `xcrun metal` on Apple runners, and against `tools/metal_stub/metal_stdlib`
  everywhere else, so a break in the Metal branch of `shim.h` fails on Linux
  too. On Apple runners it also compiles the worked example.
- `tools/package_kernels.py --verify` checks the published headers are
  byte-identical to `include/clay/kernel/` and that the package compiles with
  nothing but itself on the include path.
- `tools/check_layering.py` keeps the kernel module dependency-free, which is
  what makes the directory publishable at all.
