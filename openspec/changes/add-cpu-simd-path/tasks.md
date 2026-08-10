# Tasks: add-cpu-simd-path

- [ ] 1.1 DECIDE and record in `design.md`: xsimd only, or Apple `simd` on Apple platforms as well; and packet width — fixed 4 or the architecture's native batch. Decide on a measurement on arm64, not on the desktop
- [ ] 1.2 Audit every tape opcode for lane-evaluability BEFORE writing the evaluator. Produce the list: lane-evaluated, or per-lane scalar fallback with the reason. Data-dependent early-outs and the sampled-volume lookup are the suspected awkward ones
- [ ] 1.3 Baseline on `main`: `BM_EvalPoints`, `BM_BrickFill`, and a single-brick 8³ fill, on x86-64 and on arm64. These are the numbers the change is for
- [ ] 1.4 Packet evaluator: one walk of the tape per packet of N points, over the same kernel maths. `eval_points_reference` is not modified
- [ ] 1.5 Batch entry points feed packets and handle the remainder, so lane width is invisible above the backend
- [ ] 1.6 Parity test wired into the existing suite: every kernel, packet path vs scalar, 1e-6 relative on distances, exact on colors. This is the scenario the spec has claimed since v1 and has never had
- [ ] 1.7 Ragged-batch test: a batch of `width*k + r` points for every r in [1, width) matches an aligned batch element for element
- [ ] 1.8 Gradient path in packets (four tetrahedron taps as four packets of the same points), measured separately — the Metal backend falls back to the CPU for gradients, so this is on that path too
- [ ] 1.9 Brick-fill benchmark: the 8³ single-brick fill is the workload the sculpting path actually runs. Report speedup on arm64 and x86-64 separately; the arm64 number is the one that matters
- [ ] 1.10 Record which opcodes fell back to per-lane scalar, in `design.md` and in the delta spec, so a later reader knows what is left rather than assuming full coverage
- [ ] 1.11 Confirm `xsimd` is actually linked and its headers reachable from `backends/cpu` — `cmake/Dependencies.cmake` fetches it and nothing has ever included it
