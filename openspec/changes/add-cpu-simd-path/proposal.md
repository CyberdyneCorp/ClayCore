# Proposal: the CPU SIMD path the spec already promises does not exist

## Why

`evaluation-backends` has said this since v1:

> The CPU backend SHALL also provide a SIMD batch path (Apple `simd` on Apple
> platforms, SSE/NEON via xsimd elsewhere) and thread-pool dispatch; the SIMD
> path SHALL match scalar within 1e-6 relative on distances.

with a scenario gating it ("the parity suite evaluates every kernel on the SIMD
path against scalar"). None of that is true. There is no SIMD path, no parity
suite entry for one, and `xsimd` — fetched in `cmake/Dependencies.cmake` and
annotated "consumed from later task groups: xsimd (CPU SIMD path, group 4)" —
is included by nothing in the tree.

The trail is explicit rather than accidental. Task 4.3 shipped "compiler-
vectorized loops" and deferred "hand-tuned xsimd lanes" to task 10.6; task 10.6
shipped benchmarks and did not pick them up. What `cpu_backend.cpp` calls the
batch path is `eval_points_reference` — the scalar loop — sliced across threads:

```cpp
// backends/cpu/cpu_backend.cpp — the "batch" path
backends_cpu::ThreadPool::instance().parallel_for(
    q.count, 256, [&](std::size_t b, std::size_t e) {
        ...
        eval_points_reference(tape, sub, sub_out);   // scalar, one point at a time
    });
```

An unimplemented optimisation is normally a roadmap row, not a proposal. This
one is a proposal for three reasons.

**It is a spec that lies.** A requirement with a passing-looking scenario and no
implementation is worse than a missing requirement: it reads as covered.

**The CPU backend is the production path for the workload that matters.**
The 0.24.0 measurement settled that a brick is 8³ = 512 samples — too little
work to cover a GPU dispatch — so `clay_brick_cache_eval_requests` stays on
`"cpu"` and Metal only wins from 16³ up. Every brush dab therefore refills its
bricks through this scalar loop. It is not the fallback; it is the sculpting
path.

**A tape interpreter is the good case for SIMD.** Every point in a brick walks
the *same* instruction sequence — same opcodes, same order, same branches at the
tape level. That is the textbook shape for evaluating N points per instruction
instead of N instructions per point, and it is why the win here should be a
lane-count multiple rather than a few percent. On an iPad that is 4 points per
NEON register on the same core, at the same clock, inside the same thermal
envelope — the only kind of speedup a thermally limited device keeps.

## What changes

A packet evaluator: the tape interpreter walks its instructions once for a
packet of N points, with each opcode operating on N lanes. Scalar
`eval_points_reference` stays exactly as it is and remains the definition of
correctness; the packet path is a second implementation gated against it, in the
same relationship the GPU backends already have to it.

The batch entry points feed it packets and handle the remainder scalar, so lane
width is invisible above the backend.

## What it is not

**Not a change to the reference.** `eval_points_reference` is what "correct"
means for every backend in the tree, including the GPU ones. Nothing in this
change may touch it, and the 1e-6 the spec already states is the gate — where a
kernel cannot hold it, that kernel documents its own tolerance rather than the
gate being loosened.

**Not a rewrite of the kernel headers.** They are shared source compiled under
four dialects and a fifth would be a genuine cost. The packet path is a batching
layer over the same maths, which the tape interpreter is well placed to provide
because opcode dispatch is where the batching decision lives.

**Not a claim about every opcode.** Some are honestly awkward in lanes — the
sampled-volume lookup, anything with a data-dependent early out. The design must
name which opcodes go through lanes and which fall back to scalar per lane, and
a fallback must be a stated result rather than a silent one.

## Open questions

- **`xsimd` or Apple `simd`, or both.** The spec says both; a single portable
  path through xsimd is one implementation to test rather than two, and xsimd
  already maps to NEON on the target. Whether Apple `simd` buys anything over
  that on the same hardware is a measurement, not an assumption. To be decided
  in `design.md`.
- **Packet width.** Fixed 4 (NEON-shaped, portable) or the architecture's native
  width via xsimd's batch type. The second is faster on AVX-512 desktops and
  identical on the iPad; the first makes remainder handling trivial.
- **Divergence.** Tape control flow is uniform per point today, which is what
  makes this work. Any opcode that would break that must be identified before
  implementation rather than discovered during it.
- **Where gradients fit.** A gradient is four extra evaluations at tetrahedron
  taps — four packets of the same points, which is a natural fit, and worth
  measuring separately since the Metal path currently falls back to the CPU for
  gradients anyway.

## Impact

`evaluation-backends` keeps the requirement it already has and gains the
scenarios that make it real. No public signature changes and no output value
changes: the parity gate is that this path agrees with scalar, so if it lands
correctly nothing above the backend can tell it happened except by timing.
