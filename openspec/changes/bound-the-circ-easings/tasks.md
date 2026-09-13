## 1. The defect

- [x] 1.1 circ declared 39.98 / 28.26 where a 4096-point sweep measured
      90.50 / 63.99, and a denser sweep finds more without limit
- [x] 1.2 E'(t) = t/sqrt(1-t^2) is UNBOUNDED as t -> 1; sampling cannot bound
      an infinity and a 1.25x margin does not rescue it
- [x] 1.3 cregion_weight passes 1 - d/radius, so the singular argument is
      reached at the grab's CENTRE

## 2. The choice

- [x] 2.1 Option 3 (bound over the reachable interval) depends on unbuilt work
- [x] 2.2 Option 1 (refuse) breaks documents already containing such a deformer
- [x] 2.3 Option 2 taken: hold the curve short of the singularity
- [x] 2.4 Guard picked from a measured trade-off table, not chosen blind

## 3. What building it found

- [x] 3.1 Clamping ALONE breaks CLAY_EASE_INOUT: f(1) drops to 0.98586 and the
      two halves stop meeting, so the curve jumps by 0.0141 at t = 0.5. A
      difference quotient across it read 92.96 against an analytic 70.70
- [x] 3.2 Renormalising by the peak restores f(1) = 1 exactly -- the halves
      meet, and the grab's centre takes the FULL displacement again
- [x] 3.3 1 - g*g loses ~4 digits at g = 0.9999; (1-g)(1+g) does not

## 4. Tests

- [x] 4.1 The defect case is replaced by its opposite: declared >= observed
- [x] 4.2 THE EXCLUSION IS DELETED -- circ is back in the blanket safety loop,
      so every easing is covered and a new one that under-declares fails there
- [x] 4.3 Continuity across the in_out join is asserted; that is what caught
      the discontinuity
- [x] 4.4 Endpoints exact, and four interior values pinned
- [x] 4.5 Full suite 11/11, kernel dialect OK across cpu/cuda/metal and both
      amalgamations

## 5. Still open

- [ ] 5.1 NO PARITY SCENE OR DEFORMER GOLDEN USES A CIRC EASING -- which is why
      a kernel math change left the suite green. They are unverified on Metal,
      the same shape as #535 one family over. A parity scene for circ is
      follow-up work
- [ ] 5.2 The expo family is stiff at one end and still sampled; worth the same
      look
- [x] 5.3 Device gate before any tag carries this -- RAN: 7/7 sessions, 75 cases, 0 failures on iPad15,5 / iOS 26.5.2 at d391f817, tagged v0.113.0
