# Tasks: add-cpu-simd-path

> **RETARGETED.** 2026-08-21 the scoping pass measured what a tape evaluation is
> made of: the arithmetic this change was scoped to widen is 5% of it. 2026-08-22
> a prototype settled what to build instead — a BLOCKED evaluator that walks one
> instruction across a block of points, paying the 17-float parameter load and the
> 18-flop transform once per block rather than once per point. Measured at
> **8x per instruction, bit-identical to `ctape_eval`**, saturating at a block of
> about 64 points — so block size is not a constant to agonise over, and a brick's
> 512 sits in the flat region. See `design.md` and #207.
>
> Tasks 1.1-1.3 stand: the audit and the baseline under them are the evidence for
> the retarget, and 1.2's per-opcode finding is what makes 1.4 tractable. Tasks
> 1.4 onward are REWRITTEN — the originals widened lanes, and lane width is on the
> wrong side of the knee.

- [x] 1.1 DECIDE and record in `design.md`: xsimd only, or Apple `simd` on Apple platforms as well; and packet width — fixed 4 or the architecture's native batch. Decide on a measurement on arm64, not on the desktop
- [x] 1.2 Audit every tape opcode for lane-evaluability BEFORE writing the evaluator. Produce the list: lane-evaluated, or per-lane scalar fallback with the reason. Data-dependent early-outs and the sampled-volume lookup are the suspected awkward ones
- [~] 1.3 Baseline on `main`: `BM_EvalPoints`, `BM_BrickFill`, and a single-brick 8³ fill, on x86-64 and on arm64. These are the numbers the change is for
- [x] 1.4a MEASURED, in `design.md`: the volume opcode, the one the audit called
      genuinely different. A gather instruction costs ~3x an analytic one and
      blocking buys **2x on it rather than 8x** — but it block-evaluates
      bit-identically, so it needs no fallback, and a consolidated volume with
      stamps accumulating over it climbs from 2.84x at one stamp to 9.42x at a
      hundred. **This settles the fallback grain: block EVERY instruction and let
      the ones that gather win less.** A per-tape bail would drop 201 instructions
      to scalar to accommodate one of them
- [x] 1.4 DONE. `backends/cpu/tape_block.cpp` walks the tape once per block;
      `eval_points` and both grid paths go through it. Bit-identical to the scalar
      walk over the parity corpus plus gated, coloured-volume and radial-array
      documents the corpus does not contain, with four mutations verified failing.
      Measured against `main` on a quiet machine: **`BM_EvalPoints` 1.93x,
      `BM_BrickFill` 1.22x**, and 1.02x on `BM_DeepDocRefillPlanned10000`, which
      is culling-dominated and has little evaluation in it to take. Single-threaded
      the same evaluator is 5.3x on a sphere document and 2.5x on the benchmark
      document — see `design.md` for why one change has three honest numbers
- [x] 1.4b The prototype-to-backend gap, EXPLAINED and closed. Two causes, both
      recorded in `design.md`: hoisting a per-instruction property's VALUE while
      leaving the test inside the point loop gave 1.96x where selecting the LOOP
      once per block gave 6.44x on the same document; and the primitive becomes
      visible again once bookkeeping is removed, so a document of expensive prims
      wins less than one of spheres. The grid paths were NOT changed to dispatch
      coarser row runs — it was not needed once the loop selection was right, and
      raising `min_chunk` would have cost load balance on a single brick
- [ ] 1.5 Block size is not a new tuning constant, and the sweep says it does not
      need to be: everything from 64 up is within 4% of the best. Default to the
      caller's natural unit (a brick is 8^3 = 512 points) and handle a short final
      block
- [ ] 1.6 Batch and grid entry points feed blocks and handle the remainder, so block
      size is invisible above the backend
- [ ] 1.7 THEN, and only then, the distance-only value. Colour is worth ~1.4x ONCE
      BLOCKED — the blocked stack is an array across the block, so a 4-float value
      moves four times the bytes — and is not reliably measurable before that: two
      -O3 builds of the prototype put per-point colour at 1.02x and 1.36x. So this is
      sequenced AFTER 1.4 and measured on top of it, never on its own. #174 claimed
      2.4x for colour alone and withdrew it; this is the same claim and needs the
      blocked baseline under it
- [x] 1.8 Parity test wired into the existing suite: every kernel, blocked path vs
      scalar, on the standard corpus. The prototype was BIT-IDENTICAL on its subset,
      so assert identity and let the 1e-6 relative bound catch only opcodes that
      genuinely cannot be — each of which 1.11 must name
- [x] 1.9 Ragged-block test: a batch of `block*k + r` points for every r in
      [1, block) matches an aligned batch element for element
- [ ] 1.10 Re-measure the absent-feature checks on the GOLDEN CORPUS before claiming
      them. The prototype's 2.2x for skipping the deformer, repeat, gate, transition
      and volume tests, worth 1.6x, is measured on spheres and hard unions — it is
      the price of
      asking questions whose answer is always no, and a corpus that uses those
      features pays for them legitimately. Report what it is there, including if it
      is small
- [ ] 1.11 Gradient path in blocks (four tetrahedron taps as four blocks of the same
      points), measured separately — the Metal backend falls back to the CPU for
      gradients, so this is on that path too
- [ ] 1.12 Brick-fill benchmark, and an END-TO-END stamp figure beside it. A
      per-instruction number is exactly the kind that reads as a user-visible win and
      is not one: evaluation is 98% of a stamp, so 8x per instruction is at most ~4-5x
      on a stamp and only if the whole of it survives a real corpus. Report both, and
      report arm64 and x86-64 separately
- [ ] 1.13 Record which opcodes fell back to per-point scalar, in `design.md` and in
      the delta spec, so a later reader knows what is left rather than assuming full
      coverage
- [ ] 1.14 Drop `xsimd` from `cmake/Dependencies.cmake`, or say in the design why it
      is still fetched. It has been fetched and included by nothing since the CPU
      path was first scoped, and this change no longer needs it

## Notes from the scoping pass

- [x] 1.1 DECIDED, in `design.md`: compile the SAME kernel headers a second time
      through a WIDE mapping in `shim.h`, rather than writing a second evaluator
      or macro-parameterising the dialect. The shim already carries distinct
      mappings for CUDA, OpenCL, Metal and host C++; a packet mapping is one
      more, in the one target where operator overloading is available. The maths
      stays written once, which is the whole argument — a second copy of thirty
      primitives and thirty-six combines held together only by a parity suite is
      a maintenance cost for the life of the project.
- [x] 1.2 AUDITED, per function, in `design.md`. Three findings changed the plan:
      **the 146 branches in `tape.h` need no work at all** — they select on the
      OPCODE, and a packet runs one instruction stream for all its points, so
      every lane takes the same branch; **`ops.h` is effectively 36 of 36**,
      since its three branches test the blend radius, a per-instruction
      parameter; and **every loop has a per-instruction trip count**, so strokes,
      polygons, octaves and guides widen by substitution with no divergence to
      mask. The branch-to-select work is about twenty functions, and a tape of
      spheres, boxes, capsules and cylinders with add/subtract/smooth combines is
      already entirely branch-free.
      `ctape_volume` is the one genuinely different case: a per-point GATHER
      rather than a branch, always correct via per-lane scalar, and whether a
      hardware gather beats eight loads is a measurement.
- [~] 1.3 x86-64 baseline taken and recorded in `design.md`: `BM_EvalPoints`
      6.08 ms / 17.62 M points/s, `BM_BrickFill` 30.6 ms, and — the figure this
      change exists to move — **10.8 ns per tape instruction per point**,
      single-threaded, FLAT from a 160 KiB tape to a 4 MB one. That flatness was
      not the expectation: the walk was assumed to fall off a cache cliff and
      does not, so the packet path must not be sold on "one tape walk per eight
      points". The arithmetic width is the whole of the win.
      **arm64 half outstanding** — it needs the `macos-14` runner, not this
      machine.
