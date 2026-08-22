# Tasks: add-cpu-simd-path

> **RETARGETED 2026-08-21, before 1.4.** The scoping pass measured what a tape
> evaluation is made of and the arithmetic this change widens is 5% of it — see
> `design.md` and issue #207. Tasks 1.4 onward are NOT to be implemented as
> written. They are left here rather than deleted because the audit and the
> baseline under them are the evidence for retargeting, and because one part of
> the idea survives: a BLOCKED evaluator that walks one instruction across many
> points pays the 17-float parameter load and the 18-flop transform once per
> block instead of once per point. That is the same shape and a much smaller
> change, and it wins on loads rather than on lanes.


- [x] 1.1 DECIDE and record in `design.md`: xsimd only, or Apple `simd` on Apple platforms as well; and packet width — fixed 4 or the architecture's native batch. Decide on a measurement on arm64, not on the desktop
- [x] 1.2 Audit every tape opcode for lane-evaluability BEFORE writing the evaluator. Produce the list: lane-evaluated, or per-lane scalar fallback with the reason. Data-dependent early-outs and the sampled-volume lookup are the suspected awkward ones
- [~] 1.3 Baseline on `main`: `BM_EvalPoints`, `BM_BrickFill`, and a single-brick 8³ fill, on x86-64 and on arm64. These are the numbers the change is for
- [ ] 1.4 Packet evaluator: one walk of the tape per packet of N points, over the same kernel maths. `eval_points_reference` is not modified
- [ ] 1.5 Batch entry points feed packets and handle the remainder, so lane width is invisible above the backend
- [ ] 1.6 Parity test wired into the existing suite: every kernel, packet path vs scalar, 1e-6 relative on distances, exact on colors. This is the scenario the spec has claimed since v1 and has never had
- [ ] 1.7 Ragged-batch test: a batch of `width*k + r` points for every r in [1, width) matches an aligned batch element for element
- [ ] 1.8 Gradient path in packets (four tetrahedron taps as four packets of the same points), measured separately — the Metal backend falls back to the CPU for gradients, so this is on that path too
- [ ] 1.9 Brick-fill benchmark: the 8³ single-brick fill is the workload the sculpting path actually runs. Report speedup on arm64 and x86-64 separately; the arm64 number is the one that matters
- [ ] 1.10 Record which opcodes fell back to per-lane scalar, in `design.md` and in the delta spec, so a later reader knows what is left rather than assuming full coverage
- [ ] 1.11 Confirm `xsimd` is actually linked and its headers reachable from `backends/cpu` — `cmake/Dependencies.cmake` fetches it and nothing has ever included it

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
