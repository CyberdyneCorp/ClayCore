# Tasks: stop paying for the samples a brush cannot reach

## 1. Measure first, and re-measure what was already claimed

- [x] 1.1 Decompose a STEADY dab: per-sample overhead 56.8% at cell 0.01 and
      72.8% at 0.02, against 33.6% and 13.9% for the stencil taps everything
      else was aimed at.
- [x] 1.2 Count the waste directly rather than arguing it from geometry: 51-73%
      of selected bricks cannot hold a reachable sample, 62-95% of visited
      samples come back with weight zero.
- [x] 1.3 A first decomposition was WRONG -- it timed a full FieldVolume copy
      inside the "snapshot" and "traversal" figures, the very copy the previous
      change removes, and produced a 50-72% unaccounted bucket. The numbers
      above copy outside the timed region.

## 2. The base value is not looked up

- [x] 2.1 `old` is what a lookup would return: rewrite_region hands over the
      value in the brick it is writing, that brick is unwritten this pass, the
      snapshot holds the pre-pass value, and shared copies cannot disagree
      because every writer is a function of the global coordinate.
- [x] 2.2 So the lookup goes for EVERY sample, not only the zero-weight ones --
      which is more than the issue that prompted this asked for.

## 3. The weight compares squares

- [x] 3.1 Inside the full-strength radius and outside the taper both answer
      from the square. Only the taper takes a root, and it is the minority.

## 4. The selection narrows to the ball

- [x] 4.1 `FieldVolume::Region`: a box, optionally narrowed to a ball inside
      it. Constructible from an Aabb, so an operator whose region really is a
      box says nothing extra and existing callers are untouched.
- [x] 4.2 `rewrite_region` and `snapshot_region` both reject a brick the ball
      cannot reach, through one shared `meets` -- they must agree about which
      bricks are in play or the snapshot stops covering the written set.
- [x] 4.3 Sound for the reason the region limit is: the operator is the
      identity outside its region, so a brick the ball cannot reach holds
      nothing the pass may change.

## 5. Tests

- [x] 5.1 A ball region rewrites what the box around it would, and no more:
      three radii at two cell sizes, comparing `serialize()` against BOTH a
      full rewrite and the box form.
- [x] 5.2 Its own case rather than a parameter of the box test, because the box
      selects and the ball only rejects -- so a wrong rejection shows up as
      samples that SHOULD have changed and did not, which no test of the box
      form can see.
- [x] 5.3 A snapshot of a ball reads the field as it was, checked against a
      copy taken before, with the region rewritten to a constant.
- [x] 5.4 Full unit suite 1422, `clay_c_smoke` OK, `check_bench.py` OK.

## 6. Measured

- [x] 6.1 Steady dab, cell 0.01: 1.61 -> 1.00 ms, 1.61x.
- [x] 6.2 Steady dab, cell 0.02: 0.61 -> 0.23 ms, 2.71x.
- [x] 6.3 First dab, cell 0.01: 2.39 -> 1.65 ms, 1.45x.

## 7. Settled, and not to be picked up on old ratings

- [x] 7.1 Caching the relax stencil: `build_stencil` is 0.011% of a dab. Listed
      P1 in the plan this program came from; never worth doing.
- [x] 7.2 Ping-pong buffers: superseded by the region snapshot, 1.2%.
- [x] 7.3 Dropping `std::function` from rewrite: at most 8.4%, and it shares
      that with the traversal.

## 8. Not in this change

- [ ] 8.1 The brick+halo scratchpad, which is now the largest term. It should
      be measured against the profile this leaves rather than the one that
      motivated it.
