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

# arm64, 2026-08-23: the sequencing in this file was an x86-64 property

Every measurement above is one x86-64 desktop, and this file said repeatedly that
arm64 gated any published figure. Measured on an M2 Max (#207), and it changes
the plan rather than confirming it.

`ctape_eval` costs **6.28 ns per instruction on the M2 Max against ~15.0 ns on
the i9**. Most of the per-instruction bookkeeping this change is about is already
absorbed by that machine's out-of-order engine, which predicts the rest.

| | arm64 | x86-64 |
|---|---:|---:|
| V1 per point, colour carried — absent-feature checks | 1.48x | 1.6x |
| V2 per point, distance only — colour | **2.33x** | 2.2x |
| V3 blocked, colour carried | 1.69x | 5.7x |
| V4 blocked, distance only | 2.83x | 8.0x |
| **V3/V1 — blocking alone, colour held constant** | **1.14x** | **3.5x** |

Two of the three effects cross architectures nearly unchanged. **Blocking is the
one that does not**: 3.5x here, 1.14x there.

## What that retracts

**The sequencing.** This file put colour last on the grounds that it was not
measurable without a blocked baseline — 1.02x in one `-O3` build against 1.36x in
another. That instability was a property of one machine and one compiler, and it
was generalised into a rule about the work. On arm64 colour is 2.33x standalone
and reproduces to three decimal places. **Colour is the lever on the architecture
that ships; blocking is the follow-up.**

**"The primitive becomes visible again"** as a general consequence of removing
bookkeeping. It was right about x86-64, where the sphere document won most and
the mixed one least. On arm64 that ordering reverses and the spread nearly
vanishes: there is not enough bookkeeping left for its removal to dominate.

## What it does not retract

The blocked evaluator is **1.12x to 1.55x on arm64** and bit-identical — a
smaller win than x86-64 got, not a wrong one.

## Two constraints on the distance-only work, verified in the kernel

Recorded here rather than only in #220, because this is the file that will be
read when someone scopes it.

**A distance-only `add` still has to call `ctape_smin_m`.** Substituting the
plain `ctape_smin` looks free and is not bit-identical:

    csmin_quadratic     h = max(s - |a-b|, 0) / s,  early `return cmin(a,b)` when s <= 0
    csmin_quadratic_m   h = 1 - min(|a-b| / s, 1),  clamps s to 1e-20f instead

Same value, different last bit, different zero-support behaviour. This path's
whole defence is identity rather than tolerance. **Droppable: the `cmix` and the
four-times-wider stack. Not the smin.**

**`ccombine_paint` is a pure no-op for distance** — `r.d` is never assigned in
that branch, and the source says so: "color-only, field untouched". Skip it.

And one step further than #207 took it: in a paint combine `b.d` is read ONLY to
build the colour weight, so in a distance-only query **the prim that produced `b`
is dead as well**, not just the combine. That makes a tape-level dead-instruction
pre-pass the natural extension — mark instructions whose results only reach
colour, skip them — which lands hardest on the painted documents where colour
costs most. Its own measurement, not a reason to delay the straightforward
version.

## A confound to remove before it becomes a lead

#207 reports `BM_BrickFill` at 47.5 ms on the M2 Max against 30.6 ms on the i9
and concludes brick fill is bound by something evaluation work does not touch.
The conclusion may hold, but that comparison cannot support it: **`BM_BrickFill`
is threaded, and the machines are not the same width** — 24 hardware threads
against 12. Blocked on both, 42.6 ms against 24.4 ms is 1.74x, which is *less*
than pure thread-count scaling would predict.

What settles it is the measurement `batch-brick-eval` already used: CPU time over
wall time inside the evaluate call, i.e. cores actually used. That found 6.7-8.9
of 24 on x86-64 and is how the per-brick dispatch barrier was diagnosed.

## The measurements behind the section above, so they can be re-run

The section above is the conclusion; this is the artifact. Recorded because a
ratio with no method under it cannot be checked, and this change has twice had a
figure fail to reproduce.

**Provenance.** Apple M2 Max, 12 cores (8 performance + 4 efficiency), macOS 26,
AppleClang, `cpu-only` preset, Release. `pre-218` is **`fd70cf4`**, the commit
immediately before the blocked evaluator merged; `blocked` is **`fc6d96a`**. Both
configured and built identically, and the machine was quiet — an earlier attempt
at these was taken while ZBrush held five cores and is not what is reported here.

**The four variants.** `tape_block_prototype 2500 20480 512 sphere`, three runs,
spread under 1%, every variant asserted bit-identical to V0:

| | ns/instruction | vs V0 |
|---|---:|---:|
| V0 `ctape_eval` | 6.28 | 1.00x |
| V1 per point, colour carried | 4.23 | 1.48x |
| V2 per point, distance only | 2.69 | 2.33x |
| V3 blocked, colour carried | 3.71 | 1.69x |
| V4 blocked, distance only | 2.22 | 2.83x |

The same harness on the consolidated-volume document gives 1.41x for V4, and on
`mixed` — the volume with stamps accumulating over it — 2.83x.

**The shipped evaluator, single-threaded.** `eval_points_reference` against
`eval_points_blocked` over the same points, identity asserted before any timing
is printed, best of 9, four runs:

| document | instrs | arm64 | x86-64 |
|---|---:|---:|---:|
| the benchmark document | 47 | 1.53-1.58x | 2.5x |
| 12 spheres, hard unions | 23 | 1.42-1.51x | 4.7x |
| 200 spheres, hard unions | 399 | 1.41-1.49x | 5.3x |

**The gated benchmarks.** Medians of 11, coefficient of variation at or under
1.2% on both sides:

| benchmark | pre-218 | blocked | arm64 | x86-64 |
|---|---:|---:|---:|---:|
| `BM_EvalPoints` | 5.129 ms | 3.321 ms | 1.54x | 1.93x |
| `BM_DabRefillDenseDoc` | 1.166 ms | 0.946 ms | 1.23x | 1.84x |
| `BM_BrickFill` | 47.502 ms | 42.555 ms | 1.12x | 1.22x |
| `BM_MeshBricksGradDenseDoc` | 5.760 ms | 5.121 ms | 1.12x | 1.25x |
| `BM_DeepDocRefillPlanned10000` | 0.689 ms | 0.698 ms | 0.99x | 1.02x |

Cross-machine readings of the absolute milliseconds are confounded by thread
count — see "A confound to remove before it becomes a lead" above, which is about
exactly this table.

The `BM_MeshBricksGradDenseDoc` / `BM_DabRefillDenseDoc` ratio gate, raised to
14.0 for the x86-64 figure, sits at **5.37x** here. Left alone: the ceiling has to
hold on both architectures and the tighter side is what sets it. `ctest` is 5/5
on arm64, including the distance-only identity assertions added by the task 1.7
attempt.

## The distance-only prize, in the same two columns

The task 1.7 attempt puts the distance-only win at **2.2x on top of the blocked
path** on x86-64. Its arm64 counterpart is V3 -> V4 in the table above, which is
the same comparison — a blocked walk carrying colour against one that does not:

| | blocking, as shipped in #218 | distance-only, on top of it |
|---|---:|---:|
| x86-64 | 3.5x | **2.2x** |
| arm64 | **1.14x** | **1.67x** |

**On x86-64 the distance-only prize is the smaller of the two effects. On arm64
it is the larger one — bigger than everything #218 bought.** So the filed kernel
change is worth more on the architecture that ships than its own x86-64
measurement suggests, and should be scoped with both columns.

One prediction, recorded so it can be checked rather than assumed: the **0.57x**
measured for the rebuild-a-`CTapeValue` route should be *worse* on arm64, not
better. That route adds bookkeeping, and the finding above is that this machine
was already absorbing the bookkeeping the x86 box was paying for — the regime
where adding some costs relatively more. Not measured.

## The iPad, which is task 1.12 and is NOT answered

Two runs on the reference iPad (iPad15,5, iPadOS 26.5.2) against the committed
pre-#218 baseline, and **neither can carry a published number**:

- **Run 1** passed, but the canary reported `CONDITIONS CHANGED — x1.49 across
  the run` while `thermalState` read `nominal` at both ends.
- **Run 2**, after a 7-minute cooldown, is explicitly `INVALID: thermal nominal
  -> serious`. A gallery test crashed under the heat and `xcodebuild` exited 65,
  so the collect step never wrote a record — the salvage path
  (`collect_device_bench.py` against the result bundle) is what recovered it.

| case | pre-#218 | run 1 (valid, drift-flagged) | run 2 (INVALID, hot) |
|---|---:|---:|---:|
| `sdf_stamp_cpu` | 5.79 ms | 3.52 ms — 1.64x | 4.15 ms — 1.39x |
| `sdf_stamp_bricks` | 3.11 ms | 2.29 ms — 1.36x | 2.29 ms — 1.35x |
| `stroke_carve` | 1.49 ms | 0.87 ms — 1.71x | 0.93 ms — 1.60x |
| `sdf_consolidate` | 661.0 ms | 427.0 ms — 1.55x | 447.0 ms — 1.48x |
| `sdf_stamp_metal` — **control** | 2.00 ms | 2.03 ms — 0.99x | 2.02 ms — 0.99x |
| `voxel_mesh_dirty` — **control** | 2.11 ms | 2.14 ms — 0.99x | 2.12 ms — 1.00x |

The controls are the only reason this is worth recording. `sdf_stamp_metal` and
the `voxel_*` cases go through no part of what #218 changed, and they sit at
0.99-1.00x on **both** runs including the thermally invalid one. Heat moved every
CPU case and neither control, which is what distinguishes the CPU movement from
the thermals — and since throttling compresses the win, 1.39x is a floor and
1.64x the better estimate.

**Task 1.12 stays open.** What it needs is a properly rested device: a full run is
four to five minutes of sustained load on a passively cooled tablet, the OS
thermal signal lags it badly, and seven minutes between runs was not close to
enough. Budget half an hour.

# Task 1.7 built, 2026-08-23: the combine splits, and the coloured path got faster too

#220's `ctape_combine_dist` is in, and the CPU blocked evaluator uses it whenever
`out.colors_rgb == nullptr`. Two results, one of them not predicted by anything
above.

Read it against the arm64 column recorded above: distance-only is **1.67x on top
of blocking there against 2.2x here**, while blocking itself is 1.14x there
against 3.5x. So on the architecture that ships this is the LARGER of the two
effects — bigger than everything #218 bought — and the x86-64 figures below
understate what it is worth.

The prediction recorded alongside that column, that the rebuild-a-`CTapeValue`
route measured at 0.57x here would be *worse* on arm64, is now moot for shipping:
that route is not what landed. It stands as an unmeasured prediction about a road
not taken.

## One definition, not two

`ctape_combine_values` CALLS `ctape_combine_dist` for its distance and keeps only
the colour logic. So each mode's distance has exactly one definition and the two
cannot drift — the hazard that made this look like a bigger change than it is.
The `add` case evaluates `ctape_smin_m` a second time for the blend weight its
colour needs; both calls are pure with identical arguments, so a compiler that
does common-subexpression elimination costs the coloured path nothing.

Both constraints from #207 are honoured: the `add` distance calls `ctape_smin_m`
rather than the plain `ctape_smin` (different `h` formulation, different
zero-support guard, different last bit), and `ccombine_paint` returns the
accumulator untouched.

## The measurement, with its own control

`st_main` has no distance-only path, so ITS coloured-vs-distance-only ratio must
read 1.00x. It reads **1.000**, which is what makes the rest trustworthy.
Interleaved, five rounds, 12 spheres, 100k points, single-threaded:

| | main | this change | |
|---|---:|---:|---:|
| coloured | 7.22 ms | **5.83 ms** | **1.24x faster** |
| distance only | 7.22 ms | **3.82 ms** | **1.89x faster** |

Ranges do not overlap on either row (main 7.22-7.26, coloured 5.79-6.16,
distance-only 3.82-3.99). At 200 spheres the same shape: 104.95 -> 82.7 coloured,
-> 65.6 distance-only.

## The unpredicted half: splitting made the COLOURED path 1.24x faster

Nothing above expected this, and the honest reading is that the prediction was
about the wrong thing. Extracting the distance leaves `ctape_combine_values` as a
chain of mostly EMPTY branches — six of its thirteen modes now do nothing but
fall through — and a tight distance function beside it. The compiler lays that
out better than the interleaved original, and it more than pays for the second
`ctape_smin_m`.

So the change is not a trade of colour cost against distance cost. It is faster
on both paths, and the coloured half of that was free.

Worth stating because the reasoning that led here was wrong in a useful way:
this was scoped as "stop distance-only queries paying for colour", and the
mechanism that delivers it — one small hot function instead of one large mixed
one — helps the coloured queries too.

## End to end, on a quiet box

Medians of 11, cv under 3.5% on every row of both runs. A first attempt at this
table was taken while the box was busy and returned cv of 20-26%; those numbers
were discarded rather than published.

| benchmark | main | this change | |
|---|---:|---:|---:|
| `BM_EvalPoints` | 3.32 ms | 2.62 ms | **1.27x** |
| `BM_DabRefillDenseDoc` | 0.553 ms | 0.475 ms | **1.16x** |
| `BM_BrickFill` | 24.0 ms | 22.4 ms | **1.07x** |
| `BM_MeshBricksGradDenseDoc` | 5.65 ms | 5.43 ms | **1.04x** |

Smaller than the 1.89x the single-threaded measurement shows, and the gap is the
usual one this change keeps meeting: these paths are already threaded, and they
contain work — culling, compilation, dispatch — that this does not touch. The
ordering is the informative part. `BM_EvalPoints` is nearly pure evaluation and
gains most; `BM_MeshBricksGradDenseDoc` gains least because a gradient is four
taps plus buffer traffic around them.

The `BM_MeshBricksGradDenseDoc` / `BM_DabRefillDenseDoc` ratio gate reads
**11.4x** against its 14.0 ceiling — it moved again, for the same benign reason
as last time (the denominator gains more), and it still clears.

## `BM_BrickFillCores`, for the arm64 question

Added here rather than in its own change because it exists to answer #207's open
question and should travel with the work that raised it. It reads **15.6 cores of
24** on this box. Against 12 threads on an M2 Max, a similar fraction means brick
fill is simply narrower there; materially less means a real second bound, and
`add-mobile-thread-scheduling` — 12/19, no QoS set anywhere, efficiency cores
counted as equal workers — is where to look first.

# Task 1.12 answered, 2026-08-23: #223 on the reference iPad, A against B

The section above ("The iPad, which is task 1.12 and is NOT answered") left this
open for one reason: no run had been thermally trustworthy, and seven minutes of
cooldown was not close to enough. This is the run it asked for, and it is an A/B
rather than a comparison against the committed baseline — both sides measured on
the same device about an hour apart, so nothing rests on a record taken on
another day.

`tests/device/` and `tools/` are **byte-identical** between the two commits, so
the only thing that differs is `libclaycore.a`.

- **A** — `32b8c4c`, pre-#223. `valid: true`, thermal `nominal -> nominal`, 59 cases.
- **B** — `444efea`, post-#223. `nominal -> serious`; `testVolumeVerbsAndMaskingGallery`
  crashed and `xcodebuild` exited 65, so the record is the salvage path
  (`collect_device_bench.py` against the bundle). 54 cases.

B ran the hotter of the two. Every figure below is therefore a floor on the wins
and, if anything, generous to the regression.

## The controls, which are why the rest can be read

| control | pre | post | A/B |
|---|---:|---:|---:|
| `voxel_fill_cavities` | 0.218 ms | 0.211 ms | 1.03x |
| `voxel_mesh_dirty` | 2.126 ms | 2.121 ms | 1.00x |
| `voxel_add_level` | 4.791 ms | 4.779 ms | 1.00x |
| `voxel_mesh_whole` | 13.151 ms | 13.264 ms | 0.99x |
| `sdf_stamp_metal` | 2.009 ms | 2.105 ms | 0.95x |

Centred on 1.00x. Heat moves these; what follows moved without them.

## The end-to-end arm64 figure task 1.12 asked for

| case | pre-#223 | post-#223 | |
|---|---:|---:|---:|
| `sdf_stamp_cpu` | 3.846 ms | 2.889 ms | **1.33x** |
| `sdf_stamp_bricks` | 2.253 ms | 1.692 ms | **1.33x** |
| `sdf_consolidate` | 435.6 ms | 338.2 ms | **1.29x** |
| `stroke_carve` | 0.918 ms | 0.729 ms | **1.26x** |

**1.26-1.33x on the hardware that ships**, against the **1.67x** the arm64
microbenchmark above projected for distance-only on top of blocking. The gap is
the one this change keeps meeting: a device case is a whole verb, and the parts
of it that are not tape evaluation do not move. `sdf_consolidate` was measured
27 s later in B than in A, which makes its 1.29x the firmest floor of the four.

One measurement caveat worth carrying, because it cost a wrong number first
time: `sdf_stamp_cpu` read 2.446 ms in a run where it landed 9 s into the bundle
and 2.889 ms where it landed first, off the same binary. Position matters at the
size #212 says it does. The table above is cold-against-cold.

## And a regression, filed as #225

| record | commit | `mask_extrude` |
|---|---|---:|
| baseline (pre-#218) | `8d13b47` | 4403.3 ms |
| #218 run, 2026-08-22 | `fc6d96a` | 4054.6 ms |
| **run A** | `32b8c4c` | **4055.9 ms** |
| run 1 | `444efea` | 4878.1 ms |
| **run B** | `444efea` | **4992.3 ms** |

Two pre-#223 records off different builds on different days agree to **0.03%**.
Both post records sit 20-23% above, at the same position in the run, in the cool
part of the window, while the controls beside them held. It regresses across the
whole growth axis — 58.1 -> 62.8 ms, 419.0 -> 466.1 ms, 4055.9 -> 4992.3 ms.

`mask_extrude` is the one case in the suite that is a pure **scalar** tape
workload: `clay_c.cpp:5476` drives it as `[&tape](cfloat3 p) { return
tape.eval(p).d; }` — a colour computed per point and discarded, which is #220's
own complaint one level up. The distance-only specialisation is in the blocked
evaluator only, gated on `colors_rgb == nullptr`, so this caller collects none of
the win and pays the whole of the cost.

## What this corrects above

The section "The unpredicted half: splitting made the COLOURED path 1.24x
faster" is true on x86-64 and **false on arm64**. Same source compiled against
each commit's `tape.h`, interleaved, medians of 9 rounds, M-series host:

| mode mix | pre-#223 | post-#223 | |
|---|---:|---:|---:|
| `add` only | 3.40 ns/call | 5.54 ns/call | **1.63x SLOWER** |
| mixed | 3.27 ns/call | 4.98 ns/call | 1.52x slower |

The claim that carried the design — *"both calls are pure with identical
arguments, so a compiler that does common-subexpression elimination costs the
coloured path nothing"* — does not hold for AppleClang on arm64. It does not
eliminate the second `ctape_smin_m`. Restoring one `smin` for `add` while leaving
the other twelve modes on `ctape_combine_dist` recovers nearly all of it: 5.56 ->
3.71 ns against a 3.34 ns pre-#223 baseline.

`add`-only is the worse of the two mixes even though it dispatches on the first
branch either way, which rules out the split dispatch as the main cost and points
at the duplicated `smin` specifically.

**The lesson to carry forward is narrower than "measure on arm64".** It is that
"the compiler will eliminate this" is a *prediction about a toolchain*, not a
property of the code, and this evaluator has now punished an unmeasured
compiler-behaviour assumption three times — the value policy (#219), the
per-point branch hoist (#218), and this.

## What the gate would have said

Nothing. `mask_extrude` is 1.13x against the committed baseline, inside the 1.40
tolerance and well under its ~6.6 s budget. `tests/device/baseline.json` is still
pre-#218 and is now roughly 2x stale on the stamp cases, so it carries enough
absorbed slack to hide a 23% regression on a neighbouring case. Re-seeding it is
worth doing on its own account, separately from #225.

## The device is the constraint on all of this

Four full runs on 2026-08-22/23: one drift-flagged, two thermally invalid, two
with `testVolumeVerbsAndMaskingGallery` crashing under the heat. **45 minutes of
cooldown let a run START nominal but not finish that way.** The measure-bundle
cases survive this reliably — `sdf_stamp_bricks` read 1.699 and 1.692 ms across
two runs, `sdf_consolidate` 337.93 and 338.21 ms. The gallery cases do not:
`move_drags` moved 0.149 -> 0.434 ms between two runs of identical code, and
`stroke_build` spans 16.6-21.9 ms across four. Nothing from that bundle should be
read as a result until the suite is split.
# The guard, 2026-08-23: what could have caught #225, and what could not

The section above says splitting the combine made the coloured path 1.24x faster
and that the second `ctape_smin_m` was more than paid for. That was measured on
x86-64 and it is false on arm64, which is the architecture that ships: AppleClang
does not eliminate the second call. 1.63x on the combine, 1.23x end-to-end on the
device's `mask_extrude` (#225). The fix computes add's smin once.

Every bug fix here ships a regression test, and this one is a PERFORMANCE
regression, so the first question is which instrument could have seen it. The
honest answer for each of the existing ones is *none of them*, and the reasons
are specific enough to say what a new one has to be.

**The gated benchmarks in this file got FASTER across #223.** They are threaded
document workloads whose evaluation is distance-only, so they take the blocked
evaluator's specialisation and never call the coloured combine at all. The
coloured scalar walk is not on any gated CPU path.

**The device gate saw the regression and passed it.** `mask_extrude` read 1.13x
against its committed baseline — inside a 1.40 tolerance, under its budget —
while being 23% slower than the code that shipped the week before. A tighter
device baseline is worth having and is the obvious reading of that number, but it
is a SEPARATE change and it would not have made this visible from the CPU side,
which is where the defect is and where a developer can iterate on it in seconds
rather than in a half-hour thermal cycle. Filed as its own thing rather than
folded in here.

**An absolute ns/call threshold is rejected outright.** The combine measures
~2.8 ns/call on an M-series Mac and CI runners are about 3x slower — the spread
this file's own ceilings are built around. A number loose enough not to flake
there cannot see 1.6x here. It is the same argument every ceiling note above
makes, and it applies with more force at this scale, not less.

## What landed: `BM_TapeCombineAddColored` / `BM_TapeCombineAddColoredRef`

A ratio, for the reason ratios are used above: two things measured in the same
process on the same box move together when the machine changes.

The first shape tried was the obvious one — the coloured combine against the same
combine asked for a DISTANCE only. It works here (1.19x fixed, 1.88x post-#223)
and it should not ship, because the two sides are not the same work. The
distance-only side does no `cmix` and two loads instead of eight, so its healthy
ratio is whatever the machine charges for that difference. On a box with a
cheaper divide the extra ALU work stops hiding in the divide's shadow and the
healthy ratio climbs — plausibly past any ceiling that still fails on the defect.
No single number over that pair is defensible on a machine nobody has measured.

The reference that shipped does exactly the work a correct coloured add does:
one `ctape_combine_dist` — which for add IS one `ctape_smin_m` — one `cmix` of
the two colours, the same eight loads, the same four accumulator adds, the same
L1-resident operands, the same loop. The two differ only in where the mix weight
comes from. **The healthy ratio is ~1.0 by construction rather than by
measurement**, so runner speed cannot move it and nothing but the kernel doing
extra work can.

M-series Mac, medians of three at CI settings (`--benchmark_min_time=0.2s`):

| | coloured | reference | ratio |
|---|---:|---:|---:|
| fixed | 2.81 ns/call | 2.89 ns/call | **0.96 - 0.98x** |
| post-#223 | 4.55 ns/call | 2.92 ns/call | **1.55 - 1.56x** |

The reference does not move between the two builds — 2.86 to 2.93 ns/call either
way — which is the control the pair needs: the whole of the defect lands in the
ratio. `check_bench.py` holds it at **1.25x**, roughly 25% clear on both sides.
That is far tighter than anything else in that file and the ceiling's note says
why: the generosity elsewhere buys tolerance for a slow runner, which those pairs
do not absorb equally because they are different work. These two are the same
work.

## Two things this guard does NOT do, stated so nobody has to rediscover them

**It is blind on a toolchain that eliminates the second call.** On x86-64 both
sides are unchanged and it reads its healthy value. That is the gate working
rather than failing — it charges for the duplication on the machine that pays
for it — but it does mean the ubuntu CI job would not have caught #223. The job
that would is `release_check.py` on a developer's arm64 Mac, which is the machine
the product ships from.

**It measures THROUGHPUT, and the first version measured latency and saw
nothing.** Chained as an accumulator — each call's `a` carrying the previous
call's distance — both sides read 9.96 ns/call and the ratio was 1.02x with the
duplicate still in place. The chain runs through a divide, and a second smin
computed from the same two operands is independent of it and hides in its shadow
completely. The scalar evaluator is not in that regime: `ctape_eval` is called
once per point and the points are independent, so the machine keeps as many
combines in flight as it has room for. That is why the device saw 1.23x and a
serial probe sees nothing, and it is the trap to avoid in any future kernel
microbenchmark here.
