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
  "generator": "claycore 0.19.0",
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

The 42 cases are chosen for what a hand-written preview gets wrong rather than
for coverage of the primitive set: every blend profile against smooth union,
subtraction and intersection; all nine extended combine modes; the material-mix
weights of a colored blend; a deformer chain and a region deformer; finite
repetition; all four lift opcodes — a revolved polygon, an extruded one (what
the cut tool resolves a drawn outline into), a three-profile loft and a sweep
along a guide; a stroke chain and a closed Catmull-Rom curve; and one composed
multi-layer document with a blended mirror, a nested group and a paint pass.

Five more cover the ops and deformers added since that list was first written.
**Relief and incise** are the only ops whose second operand is a *region* rather
than geometry — a host that unions or carves it disagrees on the first probe —
and they are two cases rather than one because they share a kernel branch with
the sign taken from the mode, so a backend can reproduce one and invert the
other. **Magnify and pinch** are two cases for the same reason. **Noise** exists
mostly to catch one specific mistake: a host that reaches for the familiar
`fract(sin(dot(p, k)) * 43758.5453)` hash instead of compiling ours diverges by
O(1) rather than by a tolerance, because that hash is a chaotic amplifier and
the backends disagree about `sin` in the last place.

Each of those five is sized so it moves *some* probes and leaves others alone —
relief moves 17 of 32, magnify 14 — because the untouched probes are what
exercise the **finite support**. A case that moved every probe could not catch a
host whose falloff reaches too far.

**Every combine op the kernel implements now has a case**, and the suite checks
that by scanning the compiled tapes rather than the case names, so an op added
without one is a gate failure. Deformer kinds and primitive families are not yet
held to that standard — the set exercises 5 of 14 deformers — so a passing
fixture says less about those than it does about the combine modes.

Six of those carry data in the out-of-line blob, and two carry a lot of it:
control-point curves tessellate into the segment chain the stroke opcode
already reads, so a five-point curve reaches a host as a 35-point stroke item,
and a sweep's guide arrives as parallel-transported frames, seven floats a
vertex. Nothing about either is visible in the opcode — a host that reads the
blob by a guessed offset rather than by the item's own count passes every other
case and fails these.

**Step by `safe_step_scale`, never by 1.** A loft is a lerp of two distance
fields and a sweep compresses space on the inside of a bend, so neither is a
distance field; the tape says so, and the fixture exports it. The loft case
comes out at 0.117 and the sweep at 0.015 — a host raymarching either as if
`|∇f| = 1` steps straight through the surface. The suite asserts the fixture
contains such a case, so this cannot quietly become untestable.

The blend cases probe a ring straddling the seam of two overlapping spheres,
which is exactly where a wrong support width shows up. `tests/unit/test_parity_fixture.cpp`
asserts that a support-`k` quadratic smin — the original drift — fails the
fixture, so the gate cannot quietly stop discriminating.

Floats are written in the shortest form that round-trips, so a consumer reading
them back gets the bits claycore evaluated. Export is deterministic: two runs
of one build produce identical files, and a diff means the engine changed.

## The dialect moves, and it is versioned

Kernel headers may evolve freely within a major (`build-packaging`:
Versioning). Pin the artifact by release tag, and on an upgrade regenerate the
fixture and re-run it rather than assuming the previous one still passes.

`add-loft-opcode` is the first release where that mattered: `cop_extrude_to`
became `cop_loft`, taking the interpolation parameter instead of deriving it
from `pz`, because the old signature could only ever serve exactly two
profiles. A host calling the old name gets a compile error, which is the
outcome to want — the alternative, a silently different meaning, is the failure
this whole document is about.

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
