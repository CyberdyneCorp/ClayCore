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

---

# Revised 2026-08-21, before writing any of it

Two findings, taken in the order they were found. The second is the one that
matters.

## 1. The decision above does not survive contact with the headers

"Compile the same headers a second time through a wide mapping in `shim.h`"
assumed the dialect names its scalar through an abstraction. It does not:

```cpp
CLAY_FN float sd_sphere(cfloat3 p, float r) { return clength(p) - r; }
struct cfloat3 { float x, y, z; };
```

Every signature, every local, and every vector member says `float` — **929
occurrences across the fifteen kernel headers**. There is nothing to remap. The
option becomes "sweep 929 sites in code that four toolchains compile", which is
a different and much larger change than the one that was chosen, and it would
make every future kernel author write `cfloat` for a width they do not use.

That alone would send this back to the drawing board. What was found next means
it should not go back at all.

## 2. SIMD is the wrong target: the maths is 5% of the cost

The packet path exists to widen arithmetic. So: how much of a tape evaluation
IS arithmetic?

**Measurement one — the same maths, with and without the interpreter.** A
2,500-sphere document, 4,999 instructions, against a plain loop doing the
translate, the sphere and the min inline:

| | ns/point | per unit |
|---|---:|---:|
| `ctape_eval` | 49,695 | **9.94 ns / instruction** |
| the same maths written inline | 2,474 | **0.99 ns / prim** |

**A factor of 20.**

**Measurement two — does the primitive matter at all?** If the arithmetic were
the cost, an expensive primitive would cost more than a cheap one:

| primitive | ns/instruction |
|---|---:|
| sphere (a length and a subtract) | 10.37 |
| box | 9.84 |
| torus | 9.87 |
| round box | 9.89 |

**It does not.** A torus is two and a half times the flops of a sphere and costs
the same. The cost is FIXED PER INSTRUCTION and the primitive is invisible in it.

**Measurement three — what the fixed cost is made of**, by reading the prim
branch of `ctape_eval`. Before any primitive maths runs, every prim instruction:

- loads **17 floats** of parameter header — a 4x4 inverse matrix, a scale, a
  rounding radius and an RGB colour (`CLAY_TAPE_PRIM_HEADER == 17`)
- assembles a `cfloat4x4` on the stack and applies it to the point:
  9 multiplies and 9 adds
- pushes a `CTapeValue` — **4 floats, because colour rides through every
  instruction whether or not the caller asked for one**
- tests the repeat block, computes the deformer cursor
- and dispatches through a **switch over some thirty-five opcodes**

around roughly six flops of sphere.

So widening the arithmetic eight ways would widen five per cent of the work.
Even perfectly, that is about four per cent overall — against a change that
touches thirty primitives, twenty branch conversions and a second type mapping
in a dialect four toolchains compile.

## What this changes

**This change should not be implemented as scoped.** Not deferred for lack of
time — retargeted, because the thing it optimises is not where the time goes.

The measurements point somewhere specific instead, and it is cheaper: the
per-instruction overhead. Colour threaded through a distance-only query, a
matrix re-loaded and re-applied per point rather than per block, a stack machine
over a `CLAY_TAPE_MAX_STACK` array, and a thirty-five-way indirect branch.
Recorded as its own issue rather than folded in here.

**The one thing SIMD would still buy** is worth keeping visible, because the
argument survives in a narrower form: those 17 loads and the 18 flops of
transform are per POINT today. A blocked evaluator that walked one instruction
across a block of points would pay them once per block — and that is a win about
LOADS AND BOOKKEEPING, not about arithmetic width. It is the same shape as a
packet path and a much smaller change, and it belongs in whatever replaces this.

The three measurements above are the reason to believe any of that rather than
the reason to doubt it, and they took an afternoon. The implementation would
have taken a fortnight and returned four per cent.

# Retargeted 2026-08-22: the prototype, and what it reorders

Section 2 said the per-instruction overhead was the target and named four
suspects. Issue #207 listed them in the order it would test them, colour first.
**Prototyped, and that order is wrong.**

`benchmarks/tape_block_prototype.cpp` runs four evaluators over the same tape and
the same points, changing only the shape of the loop — the primitive and combine
maths are the shipping kernel functions (`ctape_prim_dist`,
`ctape_combine_values`, `ctape_smin_m`), so what is measured is bookkeeping. Run
it to re-derive this table rather than trusting it:

    tape_block_prototype 2500 20480 512

| | ns / instruction | vs `ctape_eval` |
|---|---:|---:|
| V0 `ctape_eval` | ~15.0 | 1.00x |
| V1 per point, colour carried | ~9.4 | 1.6x |
| V2 per point, distance only | ~6.9 | 2.2x |
| V3 **blocked**, colour carried | ~2.65 | 5.7x |
| V4 **blocked**, distance only | **~1.88** | **8.0x** |

Every variant is **bit-identical** to `ctape_eval` — the harness exits non-zero
if that ever stops being true, which is the guard that these are the same maths
and not a cheaper approximation of it. x86-64 desktop, single-threaded, and the
ratios hold at both 401 instructions (200 items, a culled brick tape) and 4,999
(2,500 items), so this is not a cache effect.

## What is robust, and what is not

The first version of this table was measured in a scratch harness built with
plain `-O3`. Rebuilt under the project's own flags the numbers MOVED, and the
discipline that matters here is separating the findings that survived that from
the ones that did not.

**Robust — reproduced in both builds:**

- **Blocking is the dominant structural win.** V1 → V3 is 3.5x and V2 → V4 is
  3.7x, in both builds. The 17-float header load and the 18-flop transform really
  are the cost, and paying them once per block is what removes them.
- **Colour is worth ~1.4x once blocked.** V3 → V4 measured 1.37x and 1.41x. In a
  blocked evaluator the stack is an array across the block, so a 4-float value
  moves four times the bytes a 1-float value does — a memory-traffic win, which
  is why it appears only in the blocked shape.
- **Bit-identity**, in every configuration tried.

**Not robust — do not plan against these:**

- **Colour BEFORE blocking measured 1.02x in one build and 1.36x in the other.**
  The colour-carrying scalar variant is simply compiled differently by the two,
  and no claim about per-point colour survives that spread. This is the number
  #207 ranked first and #174 once claimed 2.4x for; it is not measurable here at
  all. **The only place colour has a stable value is after blocking, which is
  where the task list now puts it.**
- **A sharp block-size knee.** The scratch build showed 2.2x between block 64 and
  128, which looked like a finding. The project build shows no knee — it improves
  monotonically and saturates by about 64:

  | block | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 |
  |---|---:|---:|---:|---:|---:|---:|---:|---:|
  | V4 ns / instruction | 2.58 | 2.16 | 2.32 | 1.92 | 1.89 | 1.87 | 1.86 | 1.85 |

  What survives is the useful half: **block size is not a tuning constant to
  agonise over.** Anything from 64 up is within 4% of the best, a brick's
  8³ = 512 points sits in the flat region, and even 8 gets most of it. It is
  still evidence against lane width being the story — 4 and 8 are the worst
  entries in the row — but the argument is "blocking saturates early", not "there
  is a cliff at 128".

## The cost #207 did not list

V1 differs from V0 only in that it does not check for features the instruction
does not use: the deformer chain and its cursor, the repeat block, the combine
gate, the transition test, the volume branch. Nothing but absent-feature checks,
and they are **1.6x**.

This is the most fragile number in the file and is flagged as such. It is
measured on a tape of spheres and hard unions, so it is exactly the price of
asking questions whose answer is always no. A tape that USES deformers and gates
pays for them legitimately and the margin shrinks. Task 1.10 re-measures it on
the golden corpus before any of it is claimed; it is recorded here as a lead, not
a result.

## The volume opcode, which is the case with the least evidence and the most weight

The table above is analytic prims. `clay_layer_consolidate` collapses a layer
into a single VOLUME item (`src/scene/consolidate.cpp:58`), so a consolidated
tape is one `ctape_volume` instruction whose cost is a per-point GATHER — brick
index lookup then trilinear sample — and task 1.2's audit already called that the
one genuinely different opcode. Blocking cannot hoist a gather. Measured rather
than assumed:

    tape_block_prototype 2500 20480 512 volume

| | ns / instruction | vs `ctape_eval` |
|---|---:|---:|
| V0 `ctape_eval` | ~47 | 1.00x |
| V1 per point, colour | ~41 | 1.14x |
| V2 per point, distance only | ~37 | 1.28x |
| V3 blocked, colour | ~24.4 | 1.92x |
| V4 blocked, distance only | ~23.9 | **1.96x** |

**A gather instruction costs about 3x an analytic one (47 vs 15 ns), and blocking
buys 2x on it rather than 8x.** Bit-identical, stable, and insensitive to the
volume's resolution (a 0.04 cell gives 1.82x against 0.02's 1.96x). Colour is
worth nothing here — V3 to V4 is 1.02x — because the gather dominates everything
around it.

**Two things follow, and both are good news.**

**The volume needs no fallback.** It block-evaluates correctly and bit-identically
today; it simply wins less. The spec's provision for naming opcodes that cannot be
block-evaluated stands, but the opcode most likely to have needed it does not.

**Per-tape bail would be the wrong design, and this is the measurement that says
so.** The interactive state after a consolidation is the volume with stamps
accumulating on top of it — an artist consolidates and carries on:

    tape_block_prototype <stamps> 20480 512 mixed

| stamps over the volume | 1 | 5 | 20 | 100 |
|---|---:|---:|---:|---:|
| V4 vs `ctape_eval` | 2.84x | 4.98x | 7.56x | **9.42x** |

A per-tape bail on one un-blockable instruction would drop all 201 instructions of
the 100-stamp case to scalar to accommodate one of them. **Block every
instruction, and let the ones that gather win less.**

The shape of that row is the part worth carrying: the win GROWS with the stamps
accumulating over the volume, which is precisely the accumulation that makes
sculpting slow in the first place. It is weakest immediately after a
consolidation, when the document is already cheap, and strongest as the artist
works — the right way round.

## What to predict, so it can be checked rather than celebrated

Section 2's warning applies to this change as much as to the one it replaced.
The prototype is a harness over a subset — spheres and hard unions, no
deformers, repeats, gates or volumes, single-threaded, x86-64. What it supports
is a **headroom** claim, not a delivery:

- The full evaluator must land the blocked path on the golden corpus and report
  what it actually gets there, per opcode family, including where it gets less.
- **arm64 is unmeasured**, and it is the device that matters. Task 1.3's second
  half is still outstanding and blocks any published figure.
- 8x on evaluation is not 8x on a stamp. Evaluation is 98% of a 170 ms stamp, so
  the ceiling is roughly 4-5x end-to-end and only if the whole of the 8x survives
  a real corpus. Anything published should be measured end-to-end, because a
  per-instruction figure is exactly the kind of number that reads as a
  user-visible win and is not one — which is the mistake #193 made and this
  change already made once.
- And one from this session: **quote the artifact anyone can run, not the
  scratch build.** Two `-O3` builds of identical source disagreed by 1.4x on one
  of the five variants here.
- The headline 8x is the analytic figure. A document's real multiple sits
  somewhere on the mixed row above, between 2x and 9x depending on how much has
  accumulated since its last consolidation, and any published number should say
  which document it describes.

# What landed, 2026-08-22: the evaluator, and the two things the prototype hid

Task 1.4 is implemented in `backends/cpu/tape_block.cpp` and measured against
`main`, on a quiet machine, medians of 7-11 with the coefficient of variation
under 2% on both sides unless noted.

| benchmark | main | blocked | |
|---|---:|---:|---:|
| `BM_EvalPoints` | 6.01 ms | 3.12 ms | **1.93x** |
| `BM_BrickFill` | 29.85 ms | 24.4 ms | **1.22x** |
| `BM_DeepDocRefillPlanned10000` | 0.810 ms | 0.794 ms | 1.02x |

Single-threaded, calling the two real functions directly so the thread pool is
out of the picture:

| document | ratio |
|---|---:|
| the benchmark document (torus, capped cone, octahedron, smooth blends) | 2.5x |
| 12 spheres, hard unions | 4.7x |
| 200 spheres, hard unions | 5.3x |

## 1. Hoisting the VALUE is not hoisting the BRANCH, and that was most of the win

The first working version decoded each instruction's per-block properties —
whether it has a deformer chain, a repeat block, a sampled volume — once per
block, exactly as this design said to, and then still tested them inside the
point loop and called one generic helper. It measured **1.96x** on a document the
prototype had put at 5.7x.

Selecting the LOOP once per block instead — writing out the common instruction as
its own branch-free loop, with the general path beside it — took the same
document to **6.44x**. Same hoisted values, same arithmetic, bit-identical
output; the difference was entirely whether the branch was inside the inner loop
or outside it.

Worth stating plainly because the earlier version looked like the design working.
It was 1.96x, it was correct, and it would have shipped as "blocking helps a
bit" if the prototype had not already said what the number should have been. **A
prototype's value here was as a floor to be held to, not as a prediction.**

## 2. The end-to-end figure is smaller than any of the microbenchmarks, and why

1.93x on point evaluation, 1.22x on a brick fill, and nothing measurable on the
planned refill. The three numbers disagree for three separate reasons, all of
which are the honest answer rather than a shortfall to be explained away:

**The primitive became visible again.** #207 measured a torus costing the same as
a sphere and concluded the primitive was invisible in the cost. That was true
*while bookkeeping dominated*. Blocking removes the bookkeeping, and what is left
is the arithmetic — so the win now depends on how cheap the primitives are. A
document of spheres gets 5.3x; the benchmark document's torus, capped cone,
octahedron and smooth blends get 2.5x. **This is the model confirming itself, not
contradicting itself**, but it means no single multiple describes the change and
any published figure has to name its document.

**Threading takes a cut.** The same benchmark document is 2.5x single-threaded
and 1.93x through the pool.

**`BM_DeepDocRefillPlanned10000` is not an evaluation benchmark.** Its refill is
0.810 ms against 0.841 ms for the cull alone — the culled tape is small and the
time is in planning, not evaluating. It shows 1.02x because there is almost
nothing there for this change to take. That is worth keeping in the record: the
sculpting path's remaining cost at that document size is not where this change
works.

## What is still open

- **arm64 is still unmeasured**, and it is the device that matters. Task 1.3's
  second half continues to gate any published figure.
- **Gradients are still on the scalar walk** (task 1.11).
- Task 1.10's absent-feature-check figure has now been partly answered by
  construction rather than by measurement: those checks are hoisted, and what the
  hoist is worth is inside the numbers above rather than isolated. Measuring them
  separately on the golden corpus is still worth doing.

# Task 1.7 attempted, 2026-08-23: the win is real, larger than predicted, and not reachable from the backend

Task 1.7 says colour is worth ~1.4x once blocked, so a distance-only query should
hold one float per stack slot instead of a four-float `CTapeValue`. Both halves
of that turned out to be wrong: the multiple is bigger, and the obvious way to
get it makes things slower.

## What was tried

Template the walk on a slot policy — `Coloured` stores `CTapeValue`, `DistanceOnly`
stores `float` — so one body serves both. The attraction was that it needs no
second copy of the combine semantics: `DistanceOnly` rebuilds a `CTapeValue` with
a placeholder colour, calls the SAME `ctape_combine_values`, and keeps the
distance, leaving the compiler to drop the colour arithmetic whose result is
unused.

That premise was checked first and is sound: across every combine mode, including
`replace_feather` and the gate, **no distance expression reads a colour**.
Colours are written from colours and never feed a distance. So the answers are
identical — and they were, over the whole corpus.

**It is 0.57x. Slower, not 1.4x faster.** Measured within one build, 12 spheres,
100k points, single-threaded.

These were first taken while the shared box was picking up unrelated jobs — load
average went 0.50 to 20 mid-session, one job holding eight cores — and were then
**re-taken on a quiet machine**, which is the only reason to trust them. Method,
because the first attempt got this wrong: three builds, each timing every variant
back to back in ONE process, medians of 9, and a CONTROL that runs two paths which
are the same code and must therefore return 1.00x. Under load the control returned
0.93x-1.19x; quiet it returns **1.020x, range 0.98-1.04** — so the band below is
about +/-4% and every figure here clears it by a wide margin.

| | vs coloured | range over 9 runs |
|---|---:|---|
| control — shipped, same code both ways | 1.020x | 0.98-1.04 |
| blocked, distance only, via `ctape_combine_values` | **0.560x** | 0.56-0.59 |

Rebuilding a `CTapeValue` per operand costs more than the colour it removes. The
compiler does not eliminate what the round-trip re-creates.

## What the win actually is

Replacing that one loop with a distance-only combine — `ctape_smin_m(...).x`
directly, for `ccombine_add` only, as a throwaway experiment — in the same build:

| | ms (median of 7) | |
|---|---:|---:|
| scalar reference | 34.72 | 1.00x |
| blocked, colour carried — **as shipped** | 7.79 | 4.46x |
| blocked, distance only, bypassing the combine | **3.49** | **9.95x** |

**The prize is ~2.2x on top of the blocked path we ship** (7.79 -> 3.49), and
about **10x against the scalar reference** on a distance-only query — not the
1.4x this task predicted.

Quote it against the SHIPPED coloured path, not against the experiment's own
coloured path: that build carries the templating penalty below, which would
inflate the same result to 2.8x. And 2.2x is a floor rather than a ceiling, since
the distance-only side of that build plausibly carries the penalty too. That matters more
than the multiple suggests, because the hot paths are distance-only: a brick fill
for the distance field, a raycast, and each of the four gradient taps.

Why the prototype said 1.4x: its V4 already bypassed, calling `ctape_smin_m`
directly, because its tape was hard unions. 1.4x was V3 against V4 on a tape
where the coloured path was already close to optimal — never a ceiling.

## Why it cannot be done in the backend, and what it needs instead

There is no distance-only combine in the kernel. `ctape_combine_values` is
`CTapeValue` in, `CTapeValue` out, and it is the only one. The PRIM path already
had this treatment — `speed-the-tape-prim-path` split `ctape_prim_dist` from
`ctape_volume_dist` under the heading *"forty prims are paying for one prim's
colour"* — and the COMBINE path never did.

Getting this needs the same split one level up: a `ctape_combine_dist` in the
shared kernel header, which is a five-dialect change with its own parity
obligations, not a backend task. **Filed as #220** rather than smuggled in here.

The entanglement is smaller than it looks and is worth recording for whoever
scopes it: only `ccombine_add` genuinely couples, because its colour needs the
`dm.y` that the same `ctape_smin_m` call produced. Every other mode's colour is
either decided by comparisons on distances the caller already has, or by a weight
derived from `b.d` and parameters. So one distance function plus one extra
`ctape_smin_m` in the coloured path for `add` is the likely shape.

## The templating cost the coloured path 1.3x, and that is now measured

Routing the prim loop's writes through a policy's `store()` instead of assigning
`s[j].color` and `s[j].d` directly costs **1.30x** on the coloured path.
Interleaved shipped/templated, seven rounds, quiet machine:

| build | coloured path, ms | median |
|---|---|---:|
| shipped | 7.46 - 8.92 | 7.79 |
| templated | 9.71 - 11.09 | 10.14 |

**The ranges do not overlap** — the slowest shipped round is faster than the
fastest templated one — which is what makes this a result rather than a drift.

It was first recorded as a lead and explicitly NOT a finding, because the initial
version compared two builds run at different times on a box whose load was
changing, and that comparison could not support it. Re-run interleaved on a quiet
machine, it holds. Worth keeping both facts: the number was right and the method
was not, and only one of those was visible at the time.

It is the loop-selection lesson one level down. This evaluator is unusually
sensitive to abstraction between the loop and the store, so whatever implements
the distance-only path should specialise the loops rather than the values — the
same mistake is available twice.
