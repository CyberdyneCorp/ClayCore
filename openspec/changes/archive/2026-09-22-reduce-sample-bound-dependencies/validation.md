# Validation

## Correctness and scope

- Two regression cases pass 4,092 assertions. One makes each of the 1,944 forward-neighbor pairs the unique maximum, covering every axis and row tail. The other compares 2,148 finite/random/special-value blocks with an independent flattened-neighbor oracle.
- All eleven CPU CTest suites pass (83.37 s): 2,780 C++ cases with 17,873,408 assertions, plus 757 Python tests and one Python skip.
- Focused ASan/UBSan checks with leak detection pass 105 cases and 10,086,489 assertions, covering volumes, volume culling/color, Relax, sculpt transactions and the new bound regressions.
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

Local build environments requiring the system C++ runtime use the documented LD_PRELOAD workaround. The actual-volume probe source is retained in `probes/materialization.cpp`. Compile it against each revision’s `build/cpu-only/libclaycore.a` and meshoptimizer archive, then run it with an output-file argument to produce its binary parity stream and CSV timings. The standalone probes and raw logs are retained under `/tmp/clay-531-sample-bound-production` and `/tmp/clay-531-steepest-lanes-prototype` on the validation machine.

## Combined application validation

Core `78606fc3` with desktop `4663b68` passes all 93 enabled application cases (76 library and 17 native/rendered), with two informational profiling cases ignored and no adapter skips. The engine vendor pin is restored to v0.113.0 after preserving the executable. The candidate binary SHA-256 is `bcf2c7949b1a5ce1ddb360d7bad32593a00914d9cd26765cb59adc494aaa308c`; control is the preserved row-classification binary, SHA-256 `b983000231e2337ae30394dd0b0eff222ccc0b9b1fb7686e90a5a87c70e1748d`.

Two separate 260-case comparisons each cover thirteen brushes and ten alternating pairs. One uses normal desktop scheduling; the other restricts both builds to CPUs 0,2,4,6,8,10,12,14. Both finish without command timeouts or sustained CPU contention, and every paired begin/continue/end upload count matches. Each tool starts after undoing prior history, begins at [0,0,1], continues to [0.12,0,1], then ends. CPU work from our builds and tests does not overlap either comparison.


### Normal scheduling

All values are control / updated medians in milliseconds.

| Brush | Begin | Continue | End |
|---|---:|---:|---:|
| mask | 5.747 / 5.673 | 0.017 / 0.018 | 7.408 / 7.277 |
| crease | 0.894 / 0.877 | 0.013 / 0.013 | 16.280 / 16.439 |
| clay | 1.850 / 1.848 | 0.014 / 0.014 | 13.810 / 13.650 |
| inflate | 1.647 / 1.765 | 0.018 / 0.016 | 12.933 / 13.932 |
| layer | 0.804 / 0.876 | 0.028 / 0.013 | 12.976 / 13.086 |
| standard | 1.074 / 0.959 | 0.017 / 0.013 | 13.016 / 12.928 |
| polish | 0.016 / 0.020 | 0.014 / 0.014 | 30.056 / 30.898 |
| planar | 0.022 / 0.019 | 0.012 / 0.012 | 30.436 / 29.364 |
| move-topological | 0.028 / 0.026 | 0.012 / 0.012 | 44.257 / 50.294 |
| move | 0.019 / 0.024 | 2.478 / 2.697 | 33.238 / 38.721 |
| relax | 57.917 / 58.267 | 0.031 / 0.032 | 54.945 / 54.369 |
| smooth | 53.222 / 50.414 | 0.029 / 0.025 | 55.215 / 55.673 |
| snake-hook | 0.021 / 0.020 | 11.919 / 11.828 | 22.598 / 22.559 |

Updated-build tails, also in milliseconds:

| Brush | Begin p90 / max | End p90 / max |
|---|---:|---:|
| mask | 5.824 / 6.078 | 7.423 / 7.585 |
| crease | 0.957 / 3.295 | 16.857 / 26.992 |
| clay | 8.193 / 8.560 | 23.131 / 28.928 |
| inflate | 2.105 / 3.845 | 16.821 / 24.418 |
| layer | 1.215 / 2.463 | 14.481 / 19.911 |
| standard | 1.247 / 2.065 | 13.378 / 13.432 |
| polish | 0.051 / 0.061 | 41.132 / 41.737 |
| planar | 0.022 / 0.060 | 31.301 / 33.230 |
| move-topological | 0.054 / 0.070 | 57.673 / 59.627 |
| move | 0.077 / 0.077 | 46.227 / 57.412 |
| relax | 61.792 / 70.424 | 54.871 / 57.281 |
| smooth | 52.661 / 54.566 | 61.415 / 65.288 |
| snake-hook | 0.029 / 0.044 | 30.406 / 33.864 |

### Performance cores

All values are control / updated medians in milliseconds.

| Brush | Begin | Continue | End |
|---|---:|---:|---:|
| mask | 5.686 / 5.691 | 0.022 / 0.018 | 7.345 / 7.367 |
| crease | 0.903 / 0.849 | 0.013 / 0.011 | 15.684 / 15.372 |
| clay | 1.784 / 1.731 | 0.014 / 0.014 | 13.414 / 13.162 |
| inflate | 1.610 / 2.074 | 0.012 / 0.015 | 12.981 / 13.184 |
| layer | 1.086 / 1.104 | 0.017 / 0.015 | 12.560 / 13.139 |
| standard | 0.770 / 0.935 | 0.014 / 0.015 | 12.484 / 12.442 |
| polish | 0.014 / 0.020 | 0.011 / 0.020 | 31.770 / 34.546 |
| planar | 0.016 / 0.018 | 0.011 / 0.011 | 30.087 / 29.304 |
| move-topological | 0.022 / 0.025 | 0.015 / 0.013 | 45.428 / 43.362 |
| move | 0.018 / 0.021 | 2.814 / 2.605 | 35.251 / 33.580 |
| relax | 59.266 / 56.571 | 0.025 / 0.027 | 55.265 / 55.189 |
| smooth | 54.529 / 51.029 | 0.024 / 0.029 | 55.623 / 56.878 |
| snake-hook | 0.022 / 0.027 | 12.532 / 11.920 | 21.941 / 21.909 |

Updated-build tails, also in milliseconds:

| Brush | Begin p90 / max | End p90 / max |
|---|---:|---:|
| mask | 5.990 / 6.213 | 7.596 / 9.011 |
| crease | 0.946 / 1.034 | 21.147 / 21.553 |
| clay | 3.336 / 4.371 | 17.567 / 18.569 |
| inflate | 3.745 / 7.859 | 19.660 / 23.799 |
| layer | 2.891 / 3.664 | 25.242 / 26.303 |
| standard | 2.453 / 3.208 | 17.024 / 25.588 |
| polish | 0.049 / 0.067 | 41.599 / 45.045 |
| planar | 0.032 / 0.051 | 30.995 / 32.328 |
| move-topological | 0.052 / 0.070 | 55.331 / 57.230 |
| move | 0.024 / 0.059 | 38.457 / 39.982 |
| relax | 66.527 / 66.614 | 56.717 / 57.202 |
| smooth | 52.658 / 53.459 | 71.075 / 84.468 |
| snake-hook | 0.064 / 0.066 | 25.123 / 34.221 |

The controlled comparison improves Smooth preparation 54.529 to 51.029 ms and Relax 59.266 to 56.571 ms. Normal scheduling also improves Smooth preparation, but Relax and several releases remain mixed. Polish release is slower in the controlled run. These measurements do not establish an improvement for every brush or meet the 16 ms goal. Median results are not per-run guarantees; see the tails above and the broader [measurement investigation](../../latency-investigation-531.md).

## Remaining checks

Platform CI for this revision remains pending. The 16 ms goal is still open.
