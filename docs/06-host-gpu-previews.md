# Host GPU previews — using claycore's kernels instead of copying them

A host that draws a live preview of a document while claycore bakes, meshes or
exports it has to evaluate the field twice: once on its GPU, once in the
engine. Design principle 1 ([05](05-claycore-library.md#2-design-principles))
says there is **one** implementation of every distance function and operator —
this document is how that promise reaches code outside this repository.

## Two routes, and most hosts should take the second

There are two ways to draw claycore's field on your own GPU with no risk of
drifting from it, and this document originally described only the first.

1. **Compile our kernels** into your own shader and evaluate the document's
   tape. Zero drift because it is literally the same math, and it is the only
   route that gives you an *analytic* field — refraction, arbitrary
   re-evaluation, exact normals anywhere. It needs a shading language our
   dialect compiles to. The rest of this document is that route, and since
   ABI 0.26.0 the tape it needs is reachable from C
   ([below](#getting-the-tape-of-a-live-document)).

2. **Upload the brick cache as a volume atlas** and trace it. No kernel math in
   your shader at all, therefore *also* no drift risk — and it works in any
   shading language, including WGSL, which our dialect does not target. Since
   ABI 0.25.0 this is a complete path and for most hosts it is the cheaper one.
   It is described in [the section below](#route-2-upload-the-brick-atlas).

Route 2 is sampled rather than analytic: you get the field at the cache's voxel
size, trilinearly interpolated, in a narrow band around the surface. For a
sculpting viewport that is what you were going to draw anyway. Take route 1 when
you need the field *between* the samples or *away* from the surface.

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

## Getting the tape of a live document

`clay parity-fixture` gives you tapes to test against. `clay_tape_export` gives
you the tape of the document the user is sculpting, which is what you draw.

```c
clay_tape* tape = NULL;
clay_tape_export(doc, /*region_min*/ NULL, /*region_max*/ NULL, &tape);

size_t ni, np, nb;
const clay_tape_instr* instrs = clay_tape_instrs(tape, &ni);
const float*           params = clay_tape_params(tape, &np);
const float*           blob   = clay_tape_blob  (tape, &nb);

int32_t  is_exact; float lipschitz, safe_step; float lo[3], hi[3];
uint64_t revision;
clay_tape_info(tape, &is_exact, &lipschitz, &safe_step, lo, hi, &revision);

/* upload instrs / params / blob; step by safe_step; clip against lo..hi */
clay_tape_release(tape);
```

**An export is a snapshot, and an edit cannot invalidate it.** The document
holds its compiled tape as a shared, const, revision-keyed object and installs a
*new* one on an edit rather than mutating the old, so the borrowed pointers stay
valid for the handle's lifetime and for nothing else. There is no invalidation
callback to register, no revision to re-check before dereferencing, and no
window in which a pointer you are mid-upload with goes bad. Exporting the whole
document's tape costs a refcount increment.

**Check the version.** `clay_tape_encoding_version()` returns the encoding the
running library produces; the kernel package records the encoding its headers
evaluate in `TAPE_VERSION`. They are one number because they only work together
— an opcode added on one side and absent on the other is a *wrong answer*, not
a link error. Compare them at startup and refuse on a mismatch. Neither half can
detect it for you.

**Re-upload on `revision`, not per frame.** That is the whole latency argument,
and it is worth seeing in numbers. A 512×512 preview drawn by round-tripping
through `clay_raycast_many`, against the tape that replaces it (x86-64, Release,
CPU backend):

| items | tape bytes | export | frame bytes | frame ms |
|---|---|---|---|---|
| 50 | 7 976 | 0.024 ms | 8 388 608 | 131.7 |
| 500 | 79 976 | 0.088 ms | 8 388 608 | 1 510.5 |
| 2 400 | 383 976 | 0.232 ms | 8 388 608 | 8 221.1 |

The byte ratio understates it, because the frame crosses sixty times a second
and the tape crosses once per edit. A **warm** export measures 0.000 ms: it is a
refcount increment. And the frame milliseconds are a CPU raycast your GPU would
not be running at all — which is why the round-trip is not merely wasteful but
unusable past a few hundred items.

**One caveat, on the blob.** Sampled volumes ride in it, so a document carrying
one has a blob of megabytes rather than kilobytes, and this export publishes the
whole tape with no delta encoding. Compare the *blob count* as well as the
revision: a stroke that touches no volume leaves the blob alone and you can skip
re-uploading it. A finer answer needs to know which region of a sampled volume
an edit changed, which is a property of the edit rather than of the tape, and
waits on `add-multi-resolution` and `add-consolidation-policy`.

**Culled tapes.** Pass a region and you get the same cull the brick cache uses —
useful if you are streaming. It **compiles**, where the whole-document export
does not: a culled tape is deliberately not cached, because consecutive regions
differ and a cache keyed on the document alone would thrash. One cull per brick
per frame is an expensive thing to write by accident.

## Route 2: upload the brick atlas

The brick cache already stores exactly what a GPU wants: a sparse set of `dim³`
fp16 lattices in a narrow band around the surface, in the engine's own bits. So
a host can upload the band as a sparse 3D texture atlas and sphere-trace it —
trilinear sampling plus a brick DDA, which is what `clay_brick_cache_raycast`
does on the CPU — **with no kernel math in the shader**. Nothing to drift.

This route was proposed by ClaySpaceDesktop in issue #43, which found the path
90% shipped and named the missing 10%. It is all there as of ABI 0.25.0.

**The loop.**

```c
clay_brick_config cfg;
clay_brick_config_defaults(&cfg);
cfg.colors = 1;                       /* an RGBA8 lattice beside the distances */
clay_brick_cache* cache = clay_brick_cache_create(&cfg);

/* per edit: mark -> drain -> evaluate -> submit, exactly as before, except
 * that eval_requests now also produces colours and submit takes them. */
clay_brick_cache_mark_dirty_nodes(cache, doc, layer, nodes, n, NULL);
clay_brick_cache_take_dirty(cache, reqs, &count, &remaining);
clay_brick_cache_eval_requests(doc, "cpu", reqs, count,
                               values, count * per,
                               colors, count * per * 3);
clay_brick_cache_submit(cache, reqs, count, values, count * per,
                        colors, count * per * 3, results, &accepted);

/* per upload: one memcpy per brick, into your mapped buffer, padded so the
 * hardware can filter across brick boundaries. */
clay_brick_cache_surface_bricks(cache, keys, &key_count);
clay_brick_cache_read_bricks(cache, /*lod*/ 0, keys, key_count, /*apron*/ 1,
                             states, halves, key_count * padded,
                             rgba, key_count * padded * 4);
```

**What each piece buys you.**

- **The fixed stride.** Brick `i` occupies `out_halves[i * W ...]` whatever its
  state — a uniform brick is *filled* with the band value of its sign — so the
  destination can be a mapped buffer and the upload is one memcpy with no
  packing pass and no branch on state. `CLAY_BRICK_MISSING` is the one state
  that leaves its slice untouched.
- **The apron.** `apron = 1` writes each brick as `(dim + 2)³` with a one-voxel
  halo taken from its neighbours, so `r16float` trilinear filtering is correct
  across brick faces with no manual neighbour fetches. The halo is defined
  everywhere: implicit and never-evaluated neighbours answer their band values,
  so a tile at the edge of the sculpted region filters against the band rather
  than against garbage.
- **The colour lattice.** `rgba8unorm`, filterable in WebGPU exactly as
  `r16float` is, in the same padded stride. Alpha is 255 and reserved. Colour
  is opt-in because it triples a surface brick's cost inside the same
  `memory_budget` — a colour cache holds about a third of the bricks a
  distance-only one does at the same ceiling.
- **Mips.** `clay_brick_cache_build_mip` and `lod = 1` give a coarse brick over
  2×2×2 fine ones for far-view LOD. Mips carry no colour: averaging colour over
  the block is a filtering policy, and the call refuses rather than picking one
  for you.

**Formats.** `r16float` for distance, `rgba8unorm` for colour. Both are
hardware-filterable in WebGPU, so the trilinear step is free and the shader is a
DDA over occupied bricks plus a `textureSampleLevel` per step.

**And if you still want triangles.** `clay_brick_cache_mesh` takes the key list
`clay_brick_cache_take_dirty` just handed you, so a re-mesh costs the dab rather
than the model, and reports the vertex and index range each key contributed so
you can patch sub-ranges of a GPU buffer instead of rebuilding it. Measured on
the benchmark scene: **22.6 ms** to re-mesh 232 surface bricks against
**0.64 ms** for the 8 a dab dirties. Note the documented caveat — vertex welding
spans brick seams, so a key's triangles may reference an earlier key's vertices:
you may overwrite a key's ranges, but not free them in isolation.

`clay_mesh_copy_vertices` then writes the mesh into your own mapped buffer in
your own interleaved layout, in one pass rather than an interleave into a
staging vector plus a copy.

## What is not here yet

Zero-copy, on both routes: every brick and every mesh crosses host memory,
because backends create and own their devices and there is no way to lend
claycore yours. That is `add-device-interop`.

Blob deltas, as noted above: a document carrying a sampled volume re-uploads it
whole on any edit.

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
