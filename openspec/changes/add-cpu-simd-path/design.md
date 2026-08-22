# Design: the CPU packet path

## The decision that shapes everything else

The tape evaluator is **kernel-dialect code**. `include/clay/kernel/tape.h` and
everything it calls compile as CUDA, OpenCL C99, Metal Shading Language and
Vulkan GLSL as well as C++ — `tools/check_kernel_dialect.py` compiles them all
on every push. So there are no templates to widen, and a `float8` cannot simply
be substituted for a `float` in a header that also has to be C99.

Three ways out, and the third is the one taken.

**(a) A second evaluator, written wide, in the CPU backend.** Thirty-odd
primitives, thirty-six combine ops, twenty-seven deformers, all duplicated. The
spec requires the SIMD path to match scalar within 1e-6 and the parity suite
would hold that — but only for what it covers, and it would be holding two
independent implementations of the same maths together for the life of the
project. Rejected on maintenance, not on effort.

**(b) Make the kernel headers generic.** Macro-parameterise the scalar type so
one source compiles narrow and wide. This is invasive to a dialect whose whole
value is that four toolchains agree on it, and every future kernel author pays
attention to a width they do not use.

**(c) Compile the SAME headers a second time with a wide type mapping.** This is
what `shim.h` already exists to do: it maps `cfloat3`, `clength`, `cmix` and the
rest onto whatever the current target provides, and it already has distinct
mappings for CUDA, OpenCL, Metal and host C++. A packet mapping is one more, in
ordinary C++ where operator overloading is available, so `cfloat3` becomes a
struct of three `float8` and the arithmetic follows.

The maths is written once. That is the whole argument.

## What the audit found

Per-function, over every kernel header a tape can reach. "Control flow" means an
`if` or a loop anywhere in the body; the rest is straight-line arithmetic that
widens by substitution alone.

| header | branch-free | with control flow |
|---|---:|---|
| `xform.h` | 14 / 14 | — |
| `repeat.h` | 6 / 6 | — |
| `prim3d.h` | **24 / 30** | `sd_round_cone`, `sd_round_cone_ab`, `sd_octahedron`, `sd_pyramid`, `sd_cut_sphere`, `sd_ellipsoid_bound` |
| `ops.h` | 33 / 36 | `csmin_quadratic`, `csmin_cubic`, `csmin_circular` |
| `deform.h` | 16 / 27 | `cfront_gate`, `cgrab_point`, `cmagnify_point`, `cpose_line_point`, `cpose_point`, `cbend_curve_point`, `clattice_param`, `clattice_point`, `cblob_offset`, `calpha_frame`, `calpha_offset` |
| `prim2d.h` | 6 / 10 | `sd_equilateral_triangle2`, `sd_polygon2`, `sd_polygon2_raw`, `sd_bezier2` |
| `lift.h` | 5 / 6 | `csweep_nearest` |
| `noise.h` | 5 / 6 | `cnoise_fbm` |
| `ease.h` | 9 / 11 | `cease_in_elastic`, `cease_out_bounce` |
| `stroke.h` | 0 / 1 | `sd_stroke` |

**Three findings, in order of how much they change the plan.**

**1. The opcode dispatch is UNIFORM across lanes and costs nothing.**
`tape.h` has 146 `if`s and they looked like the problem. They are not: they
select on the OPCODE, and a packet evaluates one instruction stream for all its
points. Every lane takes the same branch at every instruction. The 146 sites need
no work at all.

**2. `ops.h` is effectively 36 of 36.** Its three branches are `if (s <= 0.0f)`
on the blend radius — a per-instruction parameter, not a function of the point.
Uniform across lanes, like the dispatch. Combines are the most-executed opcode in
any tape, and they widen for free.

**3. Every loop has a per-instruction trip count, not a per-point one.**
`sd_stroke` walks the stroke's points, `sd_polygon2` its vertices,
`cnoise_fbm` its octaves, `csweep_nearest` the guide. The count comes from the
instruction's parameters, so all lanes iterate together and the loops widen by
substitution like the straight-line code. There is no lane divergence to
mask, only more work per instruction.

So the branch→select work is **about twenty functions**, and the sculpting
workload does not touch most of them: a tape of spheres, boxes, capsules and
cylinders combined with add, subtract and the smooth minima is **entirely
branch-free today**.

## The one that is genuinely different

`ctape_volume` samples a sparse brick grid, so the address depends on the point
and lanes read from different bricks. That is a GATHER rather than a branch.
It is correct either way — per-lane scalar fallback is always available — and
whether a hardware gather beats eight scalar loads is a measurement, not a
design decision. Recorded here so it is not discovered late.

## Baseline, on this machine

12th-gen i9-12900K, 8 performance + 8 efficiency cores, 24 threads, load below 3.
`main` at `57f9720`, which is after `batch-brick-eval`:

| | |
|---|---|
| `BM_EvalPoints` | 6.08 ms, 17.62 M points/s |
| `BM_BrickFill` | 30.6 ms, 37.27 k bricks/s |

And the figure the packet path exists to move, measured single-threaded so the
pool cannot flatter it — one tape instruction at one point:

| tape | size | ns/point | **ns/instruction** |
|---:|---:|---:|---:|
| 199 instrs | 16 KiB | 2,971 | 14.93 |
| 1,999 | 160 KiB | 21,601 | 10.81 |
| 4,999 | 400 KiB | 53,908 | 10.78 |
| 9,999 | 801 KiB | 108,926 | 10.89 |
| 19,999 | 1.6 MiB | 217,735 | 10.89 |
| 49,999 | 4.0 MiB | 541,365 | **10.83** |

**Flat from 160 KiB to 4 MB**, which was worth knowing and was not the
expectation: the tape walk was assumed to fall off a cache cliff and it does
not — the stream is sequential and the prefetcher covers it. So the interpreter
is compute- and dependency-bound, which is the case where widening lanes pays,
and it also means the packet path should NOT be justified by "one tape walk per
eight points". That argument is worth about nothing here; the arithmetic width
is the whole of the win.

~10.8 ns is about 50 cycles at 5 GHz for what is a transform-inverse, a distance
and a blend.

## What to predict, so it can be checked rather than celebrated

AVX2 is eight lanes and the arithmetic is wide, but the dependency chains and the
per-instruction bookkeeping are not. **3–5x on the evaluation term** is the
honest expectation. Anything near 8x would mean the scalar path was leaving
something else on the table and is worth investigating rather than accepting.

Evaluation is ~100% of an interactive stamp after `batch-brick-eval`, so that
would take a 50,000-item stamp from 77 ms to roughly 20–25 ms.
