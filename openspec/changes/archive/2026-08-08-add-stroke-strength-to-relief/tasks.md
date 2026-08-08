# Tasks: add-stroke-strength-to-relief

- [x] 1.1 A stamp's strength scales the amplitude of a relief or incise stamp
- [x] 1.2 Every other op keeps ignoring it, because their blend.k is not an amount
- [x] 1.3 Tests: buildup accumulates past clamped on a relief stroke; pressure
      moves the surface less; a boolean stroke is unchanged by either
- [x] 1.4 Docs and example

Found while building:

- [x] 1.5 A second, separate bug in the same path: `apply_stroke` never copied
      `rounding` into the stamp template at all. Relief reads it as the FALLOFF
      WIDTH, so a relief stroke declared an amplitude over ~1e-6 and the step
      scale collapsed to 2.8e-06 — the geometry was there and nothing could
      march it. The C ABI was unaffected: its template is a `clay_item`, which
      carries rounding. Fixed with a `rounding` argument matching `layer.add`,
      and a regression test.
- [x] 1.6 The example's first smoothing section asserted the ridge would drop
      with more passes. It rose. The control is the averaging RADIUS against the
      feature size, not the pass count: 4 cells at 0.012 is 0.048 world against
      a ridge 0.16 wide, which barely touches it however long it runs. Sized to
      the feature (6 cells at 0.02 = 0.12 world) the ridge goes +0.160 to +0.106,
      and at 10 cells it is gone. The example now sweeps the radius.
