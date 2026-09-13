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
- [x] 3.2 On a pinch it retries: Regularize, RegularizeLight, then 1.05x, 1.25x
      and 1.6x the target, then the welded input
- [x] 3.3 Strategy before size, because the same case needed 60% more triangles
      to clear without a flag and nothing extra with one
- [x] 3.4 An input that arrives non-manifold is returned simplified as before —
      this does not promise to repair what it did not break

## 4. Measurements

- [x] 4.1 All four failures recovered on the FIRST retry at the requested size
      (22,178 -> 22,180; 133,080 -> 133,078; 30,444 -> 30,444; 22,208 -> 22,208)
- [x] 4.2 Clean path 19.29 -> 19.97 ms, +3.5%, on 76,112 triangles in
- [x] 4.3 Regularize is NOT manifold-preserving: over 21 combinations it fixed
      cases and broke clean ones, so it is checked and never trusted

## 5. Tests

- [x] 5.1 `tests/unit/test_mesh.cpp`: the two-torus case at the ratio that
      triggers it, asserting manifold, watertight, and that the result is still
      decimated rather than the input handed back
- [x] 5.2 Verified to FAIL against the unmodified decimator, so it pins the fix
- [x] 5.3 The ratio is part of the fixture: the same document is clean at 0.05,
      0.1, 0.4, 0.6, 0.8 and 0.9, so a case at any of those would assert nothing

## 6. Still open

- [ ] 6.1 Whether a pinch can survive every retry is unmeasured — no fixture
      reached the fallback. The fallback exists because "it never happened in
      the cases tried" is not a guarantee.
- [ ] 6.2 Device gate on the reference iPad before the tag that carries this
