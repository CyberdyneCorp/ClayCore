# Validation

## Correctness and scope

- Two regression cases pass 4,092 assertions. One makes each of the 1,944 forward-neighbor pairs the unique maximum, covering every axis and row tail. The other compares 2,148 finite/random/special-value blocks with an independent flattened-neighbor oracle.
- All eleven CPU CTest suites pass (83.37 s): 2,780 C++ cases with 17,873,408 assertions, plus 757 Python tests and one Python skip.
- Local GCC and Clang probes preserve result bits and floating-point exception flags over the same 2,148 blocks under four rounding modes. This does not assert behavior with enabled traps, fast-math or every target platform.
- clang-tidy reports cognitive complexity 20 for the private helper and at most 16 for the new test functions, within the systems target. All 72 OpenSpec items pass strict validation.
- Scratch storage is four floats instead of one. The helper adds no heap allocation, persistent cache, worker or platform-specific intrinsic.

## Actual-volume comparison

The control executable links the pre-change production library at dc034eed. The candidate links the updated production library. Three alternating process pairs run six full/partial/repeated materialization fixtures seven times each; discard the first timing of each fixture when calculating medians. Processes are restricted to CPU 8, a performance core. No build/test runs overlap timing; a guard requires three quiet samples and interrupts sustained CPU contention.

All six output files match the same 143,293,584-byte payload, SHA-256 `4d0d3915440509713bba90379ea66eac6a86f5318661adc49b8362e13330c041`. The payload covers serialized volume/sample state, sample bounds, fill-callback observations, tallies and added-brick order. The synthetic block fill isolates materialization; these timings do not measure a complete application transaction.

| Fixture | Control median ms | Updated median ms |
|---|---:|---:|
| Initial full fill | 5.120 | 2.944 |
| Initial local fill | 0.202 | 0.096 |
| Full fill after local priming | 7.949 | 5.674 |
| Local fill after local priming | 0.204 | 0.097 |
| Full fill plus repeated request | 8.056 | 5.666 |
| Local fill plus repeated request | 0.207 | 0.098 |

The separate reduction-only probe improves 1,024 finite blocks from 1.571 to 0.562 ms, and 2,148 mixed/special-value blocks from 3.287 to 1.148 ms. These are component measurements, not a brush latency guarantee.

## Reproduction

```sh
cmake --build build/cpu-only -j4
ctest --test-dir build/cpu-only --output-on-failure -j4
build/cpu-only/tests/clay_unit_tests --test-case='sample_bounds:*'
cmake --build build/asan-ubsan --target clay_unit_tests -j4
openspec validate --all --strict
```

Local build environments requiring the system C++ runtime use the documented LD_PRELOAD workaround. The standalone probes and raw logs are retained under `/tmp/clay-531-sample-bound-production` and `/tmp/clay-531-steepest-lanes-prototype` on the validation machine.

## Pending

Focused sanitizer completion, combined desktop correctness, paired application timing and platform CI remain pending. The 16 ms goal is still open.
