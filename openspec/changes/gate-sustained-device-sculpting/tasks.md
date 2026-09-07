## 1. Find the axis that is missing

- [x] 1.1 AUDITED: `GrowthAxis.standard` is [10, 100, 1000] — the gate measures
      growth over DOCUMENT SIZE and nothing measures growth over SESSION LENGTH
- [x] 1.2 The failures on the second axis — a leak, an unbounded cache, a
      history outgrowing its budget, an arena that never converges — do not
      scale with the document, so no case on the first axis can reach them
- [x] 1.3 SECOND FINDING: `mesh_sculptor_stamp` was exempt as UNMEASURED
      because the harness had no mesh-layer fixture. The fixed-mesh path had
      never run on an iPad

## 2. A fixture that measures the session and not itself

- [x] 2.1 REFUTED: a Draw looped on a circle reaches 2.1x fewer vertices over 27
      revolutions, because the bump it builds lengthens the geodesic distance
- [x] 2.2 REFUTED: alternating the Draw's sign does not cancel — it deposits
      along the region's averaged normal, which the deformation turns; over 82
      revolutions the workset still fell by a third
- [x] 2.3 A Grab with the direction alternating per revolution does: 0.08% count
      drift over 29,520 dabs, p50 at 1.007x
- [x] 2.4 The tolerance is derived FROM that residue, not chosen

## 3. The CI gate

- [x] 3.1 `tests/unit/test_sustained_session.cpp`: three windows, counts,
      arena convergence, history bound. 0.15 s
- [x] 3.2 Asserts the COUNTS, not the clock — a shared runner's duration is a
      claim about its scheduler
- [x] 3.3 PROVEN TO CATCH ITS REGRESSION: fed the draw fixture it reads
      52,721 -> 29,612 and fails three assertions
- [x] 3.4 A second case pins the draw's drop, so that if a future change made a
      draw fixture stable the gate says so rather than silently over-specifying

## 4. The device leg

- [x] 4.1 `Fixture.meshLayerPatch` — a document mesh layer and a sculptor over
      it, sloped rather than flat so the normals do not all point one way
- [x] 4.2 `SustainedHarness.swift`: windows with no reset, p50/p95/p99, the
      process footprint, the thermal state and the engine's own counts
- [x] 4.3 Its own bundle and its own process, last in the run at the warm end
- [x] 4.4 `windows` on `CaseResult`, optional so every existing record decodes
- [x] 4.5 The seventh session in `run_device_bench.sh`
- [x] 4.6 The coverage exemption REMOVED, and the two beside it corrected to say
      the fixture now exists
- [x] 4.7 BUILT AND RUN ON THE REFERENCE IPAD: 720 dabs a window, counts
      56,532 / 56,532 / 56,528 (0.007% drift), p50 0.0194 / 0.0191 / 0.0190 ms,
      footprint flat at 44.4 MB, thermal nominal throughout
- [x] 4.8 The coverage guard caught the case before its table entry existed,
      which is the guard working

## 5. The verdict

- [x] 5.1 `DRIFT` in `check_device_bench.py`, beside `GROWTH`
- [x] 5.2 The counts carry it; the time carries a warning and names the thermal
      states, so a thermal reading is not read as a leak
- [x] 5.3 Footprint and arena compared MIDDLE to LATE, because the first window
      legitimately pays to build what the others reuse

## 6. Verification

- [ ] 6.1 A full seven-session gate, so the case's baseline is taken in its own
      position rather than alone and cold
- [ ] 6.2 Full unit suite green
- [ ] 6.3 `python3 tools/release_check.py --skip-slow`
- [ ] 6.4 CI green

## 8. What the gate run found about this case

- [x] 8.1 The seven-session run FAILED it: "no declared budget in the baseline".
      The case was added with its coverage exemption and no budget
- [x] 8.2 A budget would not have fixed it. The case measures 0.019 ms and a
      budget fails only when the overshoot clears NOISE_FLOOR_MS (0.05), so a
      budget at today's value could not fail until 0.069 ms -- 3.6x slower.
      That is not a loose budget; it is not a gate, and writing one would read
      as coverage
- [x] 8.3 Its real gate is `session_drift`, which ran and passed. So the
      baseline declares WHICH gate applies -- `"gate": "drift"` -- rather than
      inventing a ceiling. "Every case must declare a budget" stays true: it
      declares the gate
- [x] 8.4 The declaration is checked in both directions rather than trusted: the
      case must carry windows, and its p95 must still be under the floor. A case
      that grows into measurable territory HAS a number to gate and the
      declaration has gone stale -- the rule the binding-parity gate applies to
      its own exemptions
- [x] 8.5 Both halves proven to fire, on doctored runs: p95 forced to 0.421 ms
      reports "it has a number worth gating"; windows removed reports "nothing
      gates it"
