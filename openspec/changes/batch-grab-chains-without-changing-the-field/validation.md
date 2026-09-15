# Issue #531: exact grab-chain evaluation

## Scope and reproduction

The issue's later comments replace its original attribution: the mesh march is
flat in deformer depth, while field-gradient work grows. Cached-lattice gradients
were explicitly rejected after hard-edge errors reached about 78 degrees. This
change retains exact field gradients and every authored grab; it removes repeated
parameter decoding and invariant arithmetic from the CPU point loop.

Baseline: `main` at `6beebdd5`, using its preserved Release/GCC 13 static library.
Each comparison uses identical probe source, geometry, settings and compiler
flags, linked against the baseline and changed libraries. Final measurements ran
sequentially, without concurrent builds/tests, in three alternating before/after
runs on this Linux host. Values below are medians; they are not portable timing
thresholds or measurements of the application on the original reporting device.

## Isolated throughput

`grab_chain_batch_probe` evaluates distance, four-tap field normal, and colour on
one CPU thread. Every fixture first compares all output bits with scalar
reference evaluation. It covers chain depths 0, 1, 12 and 48, batches of 1, 4, 8,
16, 64 and 512 points, and smooth, hard-CSG, coloured-volume and mixed-chain
fixtures. This separates arithmetic from meshing, dispatch and upload costs.

| 48 grabs, 512 points | Before, µs/batch | After, µs/batch | Speedup |
| --- | ---: | ---: | ---: |
| Sphere | 1808.736 | 501.717 | 3.61× |
| Hard CSG | 1825.436 | 536.252 | 3.40× |
| Coloured volume | 1980.279 | 630.669 | 3.14× |
| Mixed-chain fallback | 1855.944 | 1824.374 | 1.02× |

At 64 points the compatible deep cases improve 3.22–3.67×. Across all 96
fixture/depth/batch combinations, no median regresses by more than 10% in this
run. Batches below four points retain the general path and do not scan the chain
for eligibility; four is the smallest tested batch using the specialization.
The mixed-chain fallback remains effectively unchanged at depth.

## Meshing and live dragging

`mesh_cost_at_depth_probe` uses the host's documented configuration: sphere,
voxel 0.02, brick dimension 8, band 3, and 5,832 bricks. At depth 48:

| Whole-form mesh | Before, ms | After, ms |
| --- | ---: | ---: |
| March only | 34.192 | 33.851 |
| Field-gradient normals | 42.364 | 39.920 |
| Gradient normals + colour | 44.261 | 39.950 |

The complete gradient mesh improves about 6%; gradient plus colour improves
about 10%. The improvement is smaller than isolated throughput because marching,
tape preparation and dispatch remain. Subtracting separate timings is noisy:
the median per-run gradient-minus-march delta is 7.329 → 5.936 ms. Do not present
the 3.6× arithmetic result as a 3.6× mesher or application speedup.

`stroke_floor_probe` uses `CLAY_PROBE_VOXEL=0.02`, `CLAY_PROBE_BAND=3` and the
specified pre-existing chain. It refills and meshes every frame of a live drag:

| Six-frame engine drag | Before, ms | After, ms |
| --- | ---: | ---: |
| Fresh sphere | 30.543 | 29.908 |
| Pre-existing chain of 48 | 51.741 | 43.412 |

The deep drag improves about 16% (8.624 → 7.235 ms/frame). The fresh-stroke floor
is effectively unchanged. These are engine-only measurements; the probe's old
comparison with historical application wall times is not used for attribution
on this different machine.

## Correctness and tooling

- All 11 CPU CTest targets pass: **2,750 C++ cases**, **756 Python tests**, one
  Python skip, plus smoke and checker targets.
- ASan, UBSan and leak detection pass on **27 evaluator/batch/backend cases**,
  **91,872 assertions**.
- Exact-output regressions cover all 33 easings, both front-gate modes, deep
  chains, transformed hard edges/corners and both sides of their discontinuity,
  coloured volumes, zero/tiny displacement, zero/negative radius, ragged blocks,
  mixed chains, radial repetition and distance-only output. Existing tests
  exercise grids, parallel batches and seeded stack evaluation.
- Layering, C ABI/shared-library FFI, imported binding parity, kernel dialect
  checks and strict OpenSpec validation pass.
- Clang-tidy cognitive complexity: every new helper is below 25. The existing
  interpreter walk decreases from **223 on main to 193** after extracting the
  primitive block. It remains above the systems target and is explicitly flagged
  as legacy interpreter debt; the full stack-machine refactor is not part of
  this latency change. Seeded evaluation remains 28 and gradient evaluation 35.
- No device kernels or public ABI change. Native GPU execution was not tested.

## Reproduce

```sh
cmake --preset cpu-only
cmake --build --preset cpu-only --target clay_unit_tests pyclay clay_c_smoke clay_cli \
  grab_chain_batch_probe mesh_cost_at_depth_probe stroke_floor_probe -j 6
ctest --preset cpu-only --output-on-failure
build/cpu-only/grab_chain_batch_probe
build/cpu-only/mesh_cost_at_depth_probe
CLAY_PROBE_VOXEL=0.02 CLAY_PROBE_BAND=3 CLAY_PROBE_CHAIN=48 \
  build/cpu-only/stroke_floor_probe
```

This machine's Conda runtime requires
`LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6` for Python/CTest to load the
module built with GCC 13. This is a test-environment setting, not a project change.

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --target clay_unit_tests -j 4
ASAN_OPTIONS=detect_leaks=1 build/asan-ubsan/tests/clay_unit_tests \
  --source-file='*test_tape_block.cpp,*test_points_batch.cpp,*test_group_resume.cpp,*test_grid_batch.cpp,*test_backend*.cpp'
```

## Application findings and remaining scope

The original common floor was reproduced through the running host's MCP
measurement path. It forced a whole-surface rebuild even for unchanged tool
selection: the regression uploaded 10,982,672 bytes before the host fix and
zero afterward. Measured edits still update geometry, and mask attribute
refreshes are included in measurement. The correction is in
CyberdyneCorp/ClaySpaceDesktop#137, alongside faster exact triangle pruning
and removal of redundant release settlement after complete replacements.

On the same v0.113.0 engine, three alternating host-only comparisons reduced
median release time from 92.324 to 8.058 ms for Mask, 161.910 to 86.118 ms for
Move, 185.545 to 112.092 ms for Relax, and 169.388 to 99.610 ms for Smooth.
Standard retained its required settlement and stayed approximately unchanged
(78.212 to 79.371 ms). These are host improvements, not engine speedup ratios.
The host PR records all 13 tools, fixture settings, commands, and limitations.

All 16 Core checks pass at `477f1b23`. The final combined host `e99ace5b` /
engine `477f1b23` application run passed all 12 native MCP, sculpt latency,
settlement, and visual brush tests on RTX 5060/Vulkan without adapter skips.

The common measurement floor is corrected, but required whole-surface
settlement and expensive region operations still exceed 16 ms. Smooth/Relax
also deliberately prime the full preview field at pointer-down; simply removing
that host pass would omit the untouched surface from its preview cache.
Compatible grab batching preserves every warp and does not bound arbitrary
chain depth or cure accumulated field degradation. Issue #531 remains open;
these changes do not establish a universal 16 ms application budget.
