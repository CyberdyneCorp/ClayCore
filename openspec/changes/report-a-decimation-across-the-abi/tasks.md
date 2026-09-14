## 1. The gap

- [x] 1.1 `grep -c DecimateReport bindings/c/clay.h` returned 0 — the report was
      C++ only while decimation itself was reachable from C
- [x] 1.2 `clay_mesh_validation_report` is not a substitute: it answers about the
      mesh in hand, and the input mesh is gone by then, so `input_manifold` is
      unrecoverable downstream at any cost
- [x] 1.3 Reproduced issue #575 through the C ABI: unit sphere, voxel 0.02,
      marching, 6 of 16 ratios from 0.20 to 0.95 pinched (45, 50, 55, 65, 70, 75)
      from an input with 0 non-manifold edges

## 2. The change

- [x] 2.1 `clay_decimate_report` — `struct_size`, `manifold`, `input_manifold`,
      `attempts` — behind the struct_size prefix rule, original layout named by
      its last field (`kDecimateReportOriginal`)
- [x] 2.2 `clay_mesh_decimate_report(const clay_mesh*, clay_decimate_report*)`
- [x] 2.3 Taken on EVERY decimated call in `mesh_tape`, so both
      `clay_document_mesh` and `clay_document_mesh_sdf_layer` carry it; the check
      was already paid for inside `decimate`
- [x] 2.4 Held on the mesh HANDLE, beside `quad_provenance`, because it describes
      a CALL and not a surface
- [x] 2.5 Carried by `clay_mesh_transform` and `clay_mesh_transform_nonuniform` —
      no index is rewritten, so no edge changes incidence
- [x] 2.6 REFUSED with `CLAY_ERROR_INVALID_ARGUMENT` for a mesh nothing
      decimated, rather than answered with a clean report

## 3. Deliberately not attempted

- [x] 3.1 Flag variations — refuted: both retries return a byte-identical mesh
- [x] 3.2 Perturbing the input — refuted: 0, 31 and 58 pinched of 76 from three
      meshes of one sphere
- [x] 3.3 Extending #549's edge guard to the tape path — refuted: 58 of 76,
      nearly double
- [x] 3.4 A repair pass — refuted on a second independent fixture: the pinches
      are flat, four triangles within ~2 degrees

## 4. Tests

- [x] 4.1 `tests/unit/test_c_decimate_report.cpp`, "a host reads the pinched
      sphere export of issue #575": the reported fixture, swept 0.20–0.95, with
      the undecimated mesh asserted clean first so a pinch is attributable
- [x] 4.2 That case asserts the AGREEMENT with the validator and the
      attribution, never WHICH ratios pinch — the mesh reaching meshoptimizer
      moves with FP contraction (281,568 vs 281,544 triangles one flag apart) and
      pinning a band failed four Linux CI jobs for `clay-desktop-team`
- [x] 4.3 "a pinch decimation was handed is not attributed to decimation": the
      deterministic half. `CLAY_MESHER_NETS` on a torus thinner than the voxel —
      624 triangles, 24 non-manifold edges, 0 boundary edges — fires on every
      toolchain and pins `input_manifold == 0` and `attempts == 1`
- [x] 4.4 "a mesh nobody decimated has no decimation to report": the refusals,
      including a mesh built from triangles and a struct_size naming no layout
- [x] 4.5 "a rigid motion carries the decimation report with the mesh"
- [x] 4.6 A `clay_mesh_decimate_report` subcase in
      `tests/unit/test_c_out_descriptors.cpp`, so an appended field cannot
      overrun a host compiled against today's layout
- [x] 4.7 PROVED by breaking the code under it: plumbing removed -> 3 of 4 cases
      fail; report derived from validating the result instead -> 1 of 4 fails on
      `input_manifold`, which is the half a host could not compute for itself

## 5. Version and gates

- [x] 5.1 ABI 0.114.0 -> 0.115.0 in `CMakeLists.txt`, `bindings/c/clay.h`
      (`CLAY_ABI_MINOR`) and `pyproject.toml`
- [x] 5.2 `tools/check_c_abi.py`, `tools/check_binding_parity.py`,
      `tools/check_test_shards.py` and `tools/release_check.py --skip-slow`
- [x] 5.3 `docs/08-mesh-readback.md` gains "What decimation did to it";
      `docs/05-claycore-library.md` states it beside decimation itself
- [x] 5.4 Built with `-Wshadow -Werror` as well as the default preset, for the
      Ubuntu GCC leg

## 6. Still open

- [ ] 6.1 The pinch itself is unfixed and this change does not claim otherwise.
      Four candidates are refuted by measurement in issue #575; the fifth —
      searching downward for the nearest clean target — needs a bound on how much
      it may shrink and is not attempted here.
- [ ] 6.2 The mesher's output is not reproducible across floating-point
      contraction settings (281,568 vs 281,544 triangles on one sphere). That is
      upstream of decimation and deserves its own issue; nothing in this
      repository currently gates it.
- [ ] 6.3 `pyclay` still discards the report. The parity gate runs pyclay -> C
      and therefore does not require it, but a Python host has the same gap this
      change closes for C.
