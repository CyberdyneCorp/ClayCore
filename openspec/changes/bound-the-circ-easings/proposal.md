## Why

**The circ easings declared a Lipschitz slope below their real one**, and a
bound below the truth does not cost frames — it costs geometry.

```
              declared   measured (4096-point sweep)
in_circ         39.98      90.50
out_circ        39.98      90.50
in_out_circ     28.26      63.99
```

`E(t) = 1 - sqrt(1 - t^2)` has `E'(t) = t / sqrt(1 - t^2)`, **unbounded as
t → 1**. No finite number of samples can bound an infinity and a 1.25× margin
does not rescue it: a denser sweep finds more again, without limit.

That slope feeds `cfi_grab`, which sets a link's Lipschitz, which sets
`safe_step_scale`, which sets how far the sphere tracer may step. **The marcher
steps past the surface** — holes, missed picks, and a sculpt that renders wrong
rather than slowly.

`cregion_weight` passes `1 - d/radius`, so the singular argument is reached at
the grab's **centre**.

## The choice, made deliberately

The issue offered three. Option 3 — a bound over the reachable interval —
depends on work that does not exist. Option 1 — refuse the combination — breaks
documents that already contain such a deformer. **Option 2** is taken: hold the
curve short of its singularity, which keeps every existing document working and
makes the bound closed form.

`CLAY_CIRC_GUARD = 1e-4`, chosen from the measured trade-off rather than picked:

| guard | declared slope | weight at centre | change |
|---:|---:|---:|---:|
| 1e-2 | 7.02 | 0.85893 | 14.11% |
| 1e-3 | 22.34 | 0.95529 | 4.47% |
| **1e-4** | **70.71** | **0.98586** | **1.41%** |
| 1e-5 | 223.61 | 0.99553 | 0.45% |

`ease_max_slope` computes its value from the **same constant**, so the curve and
its bound cannot drift apart.

## What building it found

**Clamping alone breaks the curve in two.** With the argument held short of 1,
`f(1)` is 0.98586 rather than 1 — and `CLAY_EASE_INOUT` joins `0.5*f(2t)` to
`1 - 0.5*f(2-2t)` at t = 0.5, so the halves stopped meeting and the curve
**jumped by 0.0141** there. A discontinuity is worse than a steep slope;
`cfront_gate`'s own comment says one "would wreck the Lipschitz bound sphere
tracing depends on". A difference quotient across it measured 92.96 against an
analytic 70.70, which is how it was caught.

Renormalising by the peak restores `f(1) = 1` exactly. The halves meet again,
**and the grab's centre takes the full displacement** — the guard's cost becomes
a ~1.4% reshaping spread along the curve rather than a truncation at one point.

**And `1 - g*g` is not `(1-g)(1+g)` in float.** At g = 0.9999 the first form
subtracts two nearly equal numbers and keeps about three significant digits,
which `sqrt` then amplifies — so the curve was noisy exactly where the guard
puts it.

## A coverage gap this exposed

**No parity scene or deformer golden uses a circ easing.** That is why a change
to kernel math left the whole suite green, and it means these curves are
unverified on Metal — the same shape as #535, one family over. The curve's
values are now pinned in `test_ease_slopes.cpp` because nothing else pins them;
a parity scene for the circ family is left as follow-up work.

## What changes for a caller

**Field values change for the three circ easings**, by at most ~1.4%, and by
nothing at all for every other curve. A host pinning the kernels artifact by
release tag should expect its circ rows to move.
