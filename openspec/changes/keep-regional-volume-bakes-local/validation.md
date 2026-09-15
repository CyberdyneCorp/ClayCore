# Issue #595 validation

## Reproduction

The stationary eight-subtool regression was first run against `main` at
`6beebdd5`. It failed 34 assertions: after twelve bakes the retained width was
17.6 and only one root remained. The unchanged core library was preserved for
the comparison below; both comparison executables use the same probe source.

## Repeated maintenance comparison

Release CPU build, GCC 13, Linux; eight spheres, gap 0.4, cell size 0.02,
four alternating Move gestures per bake, requested width 0.620. Times are
single-run measurements on this host, with correctness testing also running;
this is not a mobile/device performance claim.

| At gesture 48 / bake 12 | Before | After |
| --- | ---: | ---: |
| Bake time | 3396.9 ms | 33.6 ms |
| Sampled width | 16.880 | 1.120 |
| Installed volume width | 16.960 | 1.920 |
| Scene roots | 1 | 8 |
| Installed samples | 1,664,307 | 201,204 |
| Installed volume bytes | 6,947,300 | 811,016 |
| Ray hits | 1,124 | 1,124 |

The 192-gesture run passes the locality gates at every bake: eight roots,
1.120 sampled width after the initial analytic bake, 1.920 retained width,
and at most 837,260 installed bytes (207,765 samples). At gesture 192 the bake
is 35.1 ms, with 1,122 ray hits. Alternating finite deformations are not exact
inverses, so this test does not assert identical final geometry after 192 edits.
The measured safe step scale is 0.237 at gesture 48 and 0.140 at gesture 192;
local maintenance does not promise a unit slope or eliminate all marching cost.

Reproduce with:

```sh
cmake --preset cpu-only
cmake --build --preset cpu-only --target move_subtool_closure_probe -j 6
build/cpu-only/move_subtool_closure_probe --maintenance-only
build/cpu-only/move_subtool_closure_probe --maintenance-long
```

The unchanged implementation exits nonzero with these new locality gates.
The full probe also passes its separation, overlapping/blended operand, global
operand, repeated-maintenance and whole-layer comparison arms.
No timing threshold is used: deterministic root/extent checks are the gate.
Stored brick counts can change as the actual shape changes inside a fixed lattice.
Copying retained storage, rebuilding sparse bounds and measuring the final slope
still scale with retained storage; the evaluation-count regression separately
proves that distant retained geometry does not increase local field sampling.

## Correctness checks

The CPU CTest suite passes all 11 targets, including all 2,759 C++ cases and
757 Python tests (one platform-dependent skip). In this environment Conda's
older C++ runtime must be overridden with the system runtime:

```sh
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6 ctest --preset cpu-only --output-on-failure
```

Coverage includes Move followed by maintenance, retained samples and colours,
transition surface position and normals, exact shared distance halos, steep
sign crossings, all removed grab supports, undo/redo, reload, cancellation,
shared-content detachment, C/Python reporting, fallback conditions, and identical
evaluation counts for short versus distant-extended source volumes.

Architecture layering, kernel dialect profiles, C ABI hygiene plus shared-library
FFI, imported Python binding parity, task-symbol resolution and strict OpenSpec
validation pass. Clang-tidy's cognitive-complexity check finds no new production
function above 25; the measured functions in the two modified production files
remain within the systems target of 35.

AddressSanitizer, UndefinedBehaviorSanitizer and leak detection pass on 115
volume/consolidation/C-API cases (7,968,137 assertions), followed by the added
repeated-transition regression (792 assertions). The same transition test checks
surface error below 0.02 and normal dot product above 0.98 after every bake in
a 48-gesture sequence, against an independent spherical surface oracle.

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --target clay_unit_tests -j 4
ASAN_OPTIONS=detect_leaks=1 build/asan-ubsan/tests/clay_unit_tests \
  --source-file='*test_volume.cpp,*test_consolidate.cpp,*test_c_consolidate.cpp'
```

Native GPU execution was not tested; this change does not modify device kernels
or introduce a new volume representation.
