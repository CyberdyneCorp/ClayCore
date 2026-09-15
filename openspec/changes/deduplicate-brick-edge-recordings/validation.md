# Local edge recording: issue #531 follow-up

## Correctness

The reference switch is private to the meshing implementation; the public C and
C++ mesh entry points are unchanged. The reference executes the original
ShellCollector path, bypassing the dense local lookup. Regression comparisons
check complete positions, indices, normals, colours, UVs and per-brick ranges.

Fixtures combine a coloured sphere and a hard box across positive and negative
coordinates. Coverage includes full meshes, reversed key order, alternating-key
subsets and their straddlers, empty requests, both LODs, all normal modes,
dimensions 2/8/16/32, ordinary pooled calls and nested concurrent calls whose
inner march runs serially. Dimension 32 takes the direct general fallback.

- All 11 final CPU CTest targets pass (2,751 C++ cases; 756 Python tests, one
  Python skip, plus smoke/check targets).
- ASan + UBSan + leak detection: 33 relevant mesh/batch cases pass, with
  1,209,319 assertions.
- Layering and strict OpenSpec validation pass.
- Clang-tidy cognitive complexity: new helpers are below 25; the mesh orchestration
  decreases from 37 on main to 28. Boundary collection remains 30 and attribute
  evaluation 31, verified against main and within the systems target of 25–35.
- Dense scratch stores at most 17^3 * 7 uint32 indices (137,564 bytes) per active
  brick invocation. It is released before the next brick and is not retained
  across an entire recording wave. Larger dimensions skip the dense collector.

## Paired performance probe

`brick_recording_probe` measures reference and local recording in the same
Release/GCC 13 executable. Before timing it compares complete output arrays and
ranges. Five iterations alternate which path runs first. The table reports the
median of the per-process medians from three sequential runs. No builds or tests
from this validation ran concurrently; other heavy system work was present.
Absolute times are therefore informational, and these figures do not establish
a universal frame budget.

All 72 combinations completed with exact-output parity: sphere, hard box, and a
sphere with 48 grabs; dimensions 8/16/32; whole surface, 48-key, nonempty one-key
and empty subsets; no attributes and field normals plus colour. The dimension-32
reference and optimized requests both use general recording. No median above
0.2 ms regressed by more than 10% in this run. Sub-millisecond comparisons are
particularly sensitive to scheduling noise and are not portable gates.

| Whole mesh, dimension 8 | Reference ms | Local ms | Reduction |
| --- | ---: | ---: | ---: |
| Sphere, march only | 37.850 | 32.712 | 13.6% |
| Sphere, field normals + colour | 42.506 | 36.987 | 13.0% |
| Hard box, march only | 12.144 | 10.599 | 12.7% |
| Hard box, field normals + colour | 14.545 | 12.281 | 15.6% |
| 48 grabs, march only | 35.043 | 28.838 | 17.7% |
| 48 grabs, field normals + colour | 42.017 | 36.369 | 13.4% |

At dimension 16, whole meshes with attributes improve 10.4–13.5% across those
fixtures. These gains are additional to the compatible grab-chain evaluator in
this branch; the reference path here already includes that evaluator.

Temporary phase instrumentation motivated the change: serial welding was roughly
17–18 ms versus about 7 ms marching. A recent-entry cache and a replacement global
hash table both slowed welding and were discarded. Local recording reduced the
observed welding median from 18.124 to 12.886 ms in one adjacent prototype/control
pair. Production code contains neither the instrumentation nor those alternatives.

## Reproduce

```sh
cmake --preset cpu-only
cmake --build --preset cpu-only --target clay_unit_tests pyclay clay_c_smoke clay_cli brick_recording_probe -j 4
ctest --preset cpu-only --output-on-failure
build/cpu-only/brick_recording_probe
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --target clay_unit_tests -j 4
ASAN_OPTIONS=detect_leaks=1 build/asan-ubsan/tests/clay_unit_tests --source-file='*test_mesh.cpp,*test_c_brick_lod_mesh.cpp,*test_points_batch.cpp'
```

This machine needs `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6` for its
Conda Python/CTest runtime to load the GCC-built module.

## Final platform and application validation

All 16 GitHub checks passed at production revision `477f1b23`, including
Windows MSVC /WX, Linux and macOS CPU builds, Metal parity, ASan/UBSan,
ThreadSanitizer, Python, and the benchmark regression gate.

ClaySpaceDesktop `e99ace5b` was built against that exact engine revision in
Release with CPU fields and an RTX 5060 Vulkan renderer. All 12 tests passed
without adapter skips: native MCP E2E (3), sculpt latency (4), settlement (3),
and visual brushes (2). This covers both engine changes together with the host
fixes in CyberdyneCorp/ClaySpaceDesktop#137. The host's committed engine pin
remains v0.113.0; its PR also contains independent pinned-engine verification.

The broader application-level 16 ms goal in #531 remains open. The companion
host fixes remove the unconditional measurement rebuild, accelerate exact
triangle pruning, include deferred mask uploads in measurement, and avoid a
second release rebuild when synchronization already replaced the full surface.
Required settlement and expensive region operations remain above budget.
