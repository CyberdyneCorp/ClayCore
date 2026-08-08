# Tasks: add-trim-curve

- [x] 1.1 `CutShape::from_open_curve`: open tessellation, closed against the frame bounds
- [x] 1.2 The side names which half the polygon covers; the op still decides its fate
- [x] 1.3 Python bindings
- [x] 1.4 Tests: a trim removes one half and leaves the other; the two sides are
      complementary; it agrees with a hand-built polygon; a closed lasso of the
      same points is NOT the same cut; degenerate input is refused
- [x] 1.5 Docs and an example

Found while building:

- [x] 1.6 The example first asserted that covering ABOVE and subtracting is
      identical to covering BELOW and intersecting. As FIELDS they are not:
      subtract is max(a, -b) and intersect is max(a, b), which agree on the sign
      and disagree on the distance outside the surface — measured 0.554 apart.
      The claim is about which material survives, so it compares solids, and
      they agree at 100% of 5000 probes.
- [x] 1.7 The first probes sat at y = +-0.45, which is exactly the surface:
      RoundBox's `size` is the full extent, so the half-height is 0.45 and the
      field read 0.000 either side of the trim. Moved them inside.
