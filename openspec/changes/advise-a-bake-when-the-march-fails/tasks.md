# Tasks

## 1. Pin the measurement before changing anything

- [ ] 1.1 Re-run `benchmarks/move_collapse_crossover_probe.cpp` on a QUIET box
      and record the crossover. The table in the proposal was taken while a
      build ran; the shape is not in doubt but the floor is a constant and a
      constant deserves a clean room.
- [ ] 1.2 Vary OVERLAP at fixed chain depth, not just depth. Two runs with
      identical chains reached step scales of 0.296 and 0.000837 depending on
      how much the grabs shared items. Confirm the crossover is a function of
      the step scale alone — if it is not, the floor cannot live on the step
      scale and this change needs rethinking before it is built.
- [ ] 1.3 Re-derive the enum's own "6x worse, 29x better step scale" figure and
      record which step scale it was taken at. The whole proposal rests on that
      being a regime rather than a universal, and nobody has checked the number
      since it was written.
- [ ] 1.4 Vary FORM COMPLEXITY at a fixed cell size and confirm the baked step
      scale still lands on 1/sqrt(3). Cell size was measured and moves it by
      1.00x; complexity was NOT measured, and the argument that redistancing
      makes it irrelevant is reasoning. If a dense form bakes to something
      lower, the crossover moves and the floor must be derived from the
      projected bake rather than fixed.
- [ ] 1.5 Measure the floor on Metal as well as CPU. The ray/sample tradeoff is
      backend-dependent and a floor tuned on one may be wrong on the other; if
      they disagree, say so in the header rather than picking one.

## 2. The change

- [ ] 2.1 Add the floor to `include/clay/scene/consolidate.h` as a named
      constant beside the other bounds, with the table and the run that
      produced it in the comment.
- [ ] 2.2 Widen the condition in `src/scene/consolidate.cpp` to
      `degraded && (volumes || safe_step_scale < floor)`.
- [ ] 2.3 Keep `degradation` unchanged — a chain past the floor is still
      `Deformers`. The kind of degradation did not change; only whether the
      cure applies.
- [ ] 2.4 Check every existing caller of `advises_consolidation` for one that
      assumed it implies volumes. `clay_layer_consolidation_advice` is defined
      against it (`consolidate.cpp:754`) and is expected to follow; anything
      else that reads it must be looked at rather than assumed safe.

## 3. The documentation, which is half the change

- [ ] 3.1 Rewrite `CLAY_DEGRADATION_DEFORMERS` in `bindings/c/clay.h` to name
      the regime, carry the floor, and state that past some depth the march
      stops finding the surface at all.
- [ ] 3.2 State in `clay_layer_field_report`'s block how `degradation` and
      `advises_consolidation` now relate, so a host reading only one knows what
      it is missing. ClaySpaceDesktop read `advises_consolidation` and ignored
      `degradation`, which is the field that described its actual failure.
- [ ] 3.3 Say plainly that the bake is still the WRONG cure above the floor, so
      nobody reads this change as "consolidation fixes brush chains".

## 4. Tests

- [ ] 4.1 A layer either side of the floor: named `Deformers` both times,
      advised only below. Drive it with real `clay_sdf_move_*` dabs rather than
      hand-built chains, so the test degrades the way a sculptor does.
- [ ] 4.2 The advice cures what it named: consolidate at the advised parameters,
      report again, assert the step scale is above the floor and nothing is
      advised.
- [ ] 4.3 The caller's `advise_below_step_scale` does not move the floor.
- [ ] 4.4 A volumes-degraded layer is still advised exactly as before — this
      change must not alter the path it did not touch.
- [ ] 4.5 **The no-hit case.** Pin that at a low enough step scale
      `clay_raycast` returns no hit against a form that is plainly there. That
      is the strongest argument in the proposal and it is currently a printed
      observation in a probe rather than a test.
- [ ] 4.6 Prove the test can fail: set the floor to 0, confirm 4.1 goes red,
      revert. A threshold test that has only ever passed proves nothing.

## 5. What this does not close

- [ ] 5.1 Record in the proposal archive that a parametric collapse remains
      the better cure and is unbuilt, with the 16-dab no-hit row as the case
      for funding it. This change buys time; it does not fix the accumulation.
- [ ] 5.2 File the coalescing defect separately — bit-exact centre and radius,
      101x on a pressure-driven radius. Same subsystem, different bug, and it
      must not ride along in this change.
