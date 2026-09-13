## 1. The defect

- [x] 1.1 `decimate` returned edges with four incident triangles from inputs
      with none, breaching the 2-manifold promise `clay_document_mesh` makes
- [x] 1.2 MEASURED on an unmodified tree: 4 of 20 sphere-minus-box
      configurations across four voxel sizes and five ratios
- [x] 1.3 The pinned case: two crossed tori at 0.035, decimated to a quarter —
      two edges of incidence four, Euler -4 -> -2, so a handle was closed
- [x] 1.4 Cause: meshoptimizer does not apply the link condition
      `collapse_edge` refuses on (`mesh/topology_ops.h`)
- [x] 1.5 EXONERATED `weld_positions` by measuring it rather than assuming:
      its output is manifold on all three fixtures, so the input meshopt sees
      is clean

## 2. A repair pass, built and then rejected

- [x] 2.1 Implemented edge splitting by orientation and radial order, the
      standard remedy
- [x] 2.2 REFUTED by measurement: the pinches are FLAT — four triangles at
      0, 178.7, 178.7 and -177.3 degrees — so the pairing is a floating-point
      tie-break that decides the surface's genus
- [x] 2.3 The two pairings disagree: one separated nothing, the other made four
      vertex copies and a different Euler characteristic
- [x] 2.4 Deleted rather than shipped. The derivation of the correct rotational
      rule is recorded in the proposal so nobody re-derives it to the same end.

## 3. The change

- [x] 3.1 `decimate` checks its own result for edges carrying more than two
      triangles
- [x] 3.2 On a pinch it retries with a different choice of collapses AT THE SAME
      TARGET (Regularize, RegularizeLight) and prefers a clean result
- [x] 3.3 A retry is rejected if it grew the result past 1.05x
- [x] 3.4 Where nothing clean fits, the REQUESTED SIZE is returned and reported
      as pinched
- [x] 3.5 `DecimateReport` carries manifold, input_manifold and attempts, so an
      export gate can decide — the caller knows whether a smaller mesh or a
      two-sided one is worth more
- [x] 3.6 An input that arrives non-manifold is returned simplified as before —
      this does not promise to repair what it did not break

## 3b. Two versions this went through, both corrected by measurement

- [x] 3b.1 The FIRST version never returned a non-manifold mesh, falling back to
      the undecimated input. CI caught it: `run_all.py` 76/76 -> 74/76, with
      `37_groups.ply` at 4021 KiB against a 400 KiB budget
- [x] 3b.2 Premise refuted: at an aggressive ratio a pinch is not incidental.
      155,388 -> 12,418 at ratio 0.08, all six retries pinched, fallback
      returned 155,388 — a twelvefold file
- [x] 3b.3 The SECOND version kept the target but not the size. Regularize
      weighs triangle shape and stops short: 1,744 triangles where 834 were
      asked for, doubling `04_repeat_radial.ply`
- [x] 3b.4 Size relaxations (1.05x/1.25x/1.6x) deleted — every measured recovery
      came from the flags at the requested size, within two triangles
- [x] 3b.5 With the growth bound the gallery reproduces its committed element
      counts exactly, 76/76 examples run, `check_gallery.py` passes

## 4. Measurements

- [x] 4.1 All four failures recovered on the FIRST retry at the requested size
      (22,178 -> 22,180; 133,080 -> 133,078; 30,444 -> 30,444; 22,208 -> 22,208)
- [x] 4.2 Clean path 19.29 -> 19.97 ms, +3.5%, on 76,112 triangles in
- [x] 4.3 Regularize is NOT manifold-preserving: over 21 combinations it fixed
      cases and broke clean ones, so it is checked and never trusted

## 5. Tests

- [x] 5.1 `tests/unit/test_mesh.cpp`, "decimation recovers a pinch that is
      recoverable": the two-torus case at the ratio that triggers it, asserting
      manifold, watertight, still decimated, and that the report agrees
- [x] 5.2 Verified to FAIL against the unmodified decimator, so it pins the fix
- [x] 5.3 The ratio is part of the fixture: the same document is clean at 0.05,
      0.1, 0.4, 0.6, 0.8 and 0.9, so a case at any of those would assert nothing
- [x] 5.4 "an unrecoverable pinch returns the requested size and says so": the
      counterpart, pinning the behaviour CI had to teach this change — the SIZE
      is what was asked for, and the report matches the mesh

## 6. Still open

- [ ] 6.1 A pinch that no collapse order clears is NOT rare at aggressive
      ratios, and such a result is returned pinched. A caller that cannot use one
      has to read the report and decide; nothing here repairs it.
- [ ] 6.2 The 1.05x growth allowance is a judgement, not a measurement. Every
      recovery observed came in within two triangles, so no fixture exercises the
      space between.
- [x] 6.3 Device gate on the reference iPad before the tag that carries this -- RAN: 7/7 sessions, 75 cases, 0 failures on iPad15,5 / iOS 26.5.2 at d391f817, tagged v0.113.0
