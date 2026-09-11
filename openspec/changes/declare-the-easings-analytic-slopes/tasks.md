## 1. The defect

- [x] 1.1 `ease_max_slope` returned `sampled * 1.25` for every curve, linear
      included, whose true steepest slope is exactly 1
- [x] 1.2 MEASURED: the margin compounds through a chain — a 48-grab chain
      declared 1,443,067.88 against an analytic product of 116,008.14, 12.44x

## 2. The change

- [x] 2.1 The polynomial, sine, smoothstep, smootherstep and linear families
      return their analytic suprema
- [x] 2.2 circ, expo, back, elastic and bounce keep the 512-point sample and the
      1.25x margin, with the reason stated per family
- [x] 2.3 The two kinds are distinguishable in the source, so a later curve
      cannot be added to the wrong one by accident

## 3. Tests

- [x] 3.1 `tests/unit/test_ease_slopes.cpp`: every curve's declared slope is
      checked against a dense sample of its own difference quotient
- [x] 3.2 It caught a real error in the first version — in-out declared 2/4/8/16
      where the correct answer is n, and `in_out_quart` samples 4.0 against a
      declared 8.0
- [x] 3.3 Desensitised to float noise: 4,096 samples with a documented 5e-3
      allowance, after 65,536 samples "measured" sine above pi/2

## 4. A defect pinned rather than fixed

- [x] 4.1 The circ family declares BELOW its true slope: `E'(t) = t/sqrt(1-t^2)`
      is unbounded at the endpoint and no sampled value bounds it
- [x] 4.2 Filed as #543 and pinned in the test as current behaviour, with the
      exclusion marked for deletion when it is fixed

## 5. Still open

- [ ] 5.1 Device gate on the reference iPad before the tag that carries this
- [ ] 5.2 #543 itself: the circ family needs a real bound, not a margin
