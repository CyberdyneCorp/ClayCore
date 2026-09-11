## Why

**`ease_max_slope` returned a blanket 1.25x margin over a 512-point sample for
EVERY easing curve, including the ones whose derivative supremum is a known
constant.** Linear's true steepest slope is exactly 1; it was declaring 1.25.

The number compounds. It multiplies a link's Lipschitz factor in `cfi_grab`
(`1 + |d| * slope / r`), and a chain multiplies those factors, so a per-link
excess is raised to the power of the chain length. Measured on a 48-grab chain
with `benchmarks/declared_vs_actual_probe.cpp`:

```text
declared before   1,443,067.88
declared after      116,008.14      12.44x
```

A 5.4% per-link excess became 12.4x. `safe_step_scale` is `1 / max(L, 1)`, so
that is 12.44x more steps for every sphere trace through the chain.

## What this is NOT

**Not a licence to declare a sampled number.** A sampled maximum is a LOWER
bound on the true supremum, and a bound below the truth does not make the
marcher slow — it makes it step through the surface. The margin SHALL stay
wherever sampling stays.

So the curves whose supremum is known analytically return it exactly, and only
those. The circ, expo, back, elastic and bounce families keep the 512-point
sample and the 1.25x margin: circ's derivative is unbounded at its endpoint,
expo is stiff near one end, and back and elastic overshoot outside [0, 1] —
that overshoot is the curve, and deleting it is the defect #527 had to fix.

## What building it found

**The in-out forms are not steeper than the one-sided forms.** A first version
declared 2/4/8/16 for quad/cubic/quart/quint in-out while the comment beside it
derived the right answer: the first half is `2^(n-1) t^n`, so `E'` peaks at the
midpoint with `n * 2^(n-1) * (1/2)^(n-1) = n` — the SAME slope. The new test
caught it, because a dense sample of `in_out_quart` reads 4.0 against a declared
8.0.

**The test had to be desensitised before it could be trusted.** At 65,536
samples it "measured" sine above pi/2 and failed a correct bound: `cease`
returns a float and `h = 1/65536` amplifies the rounding to 8e-3. It runs at
4,096 samples with a documented 5e-3 allowance.

**It also pinned a defect rather than fixing it.** The circ family declares a
slope BELOW its real one — `E'(t) = t/sqrt(1-t^2)` is unbounded at the endpoint,
so no sampled value bounds it. That is pre-existing, is filed as #543, and is
pinned in the test as the CURRENT behaviour with the exclusion marked for
deletion when it is fixed.

## What changes for a caller

Nothing in any field's value, and nothing in the ABI. A chain of eased
deformers reports a smaller `lipschitz` and a larger `safe_step_scale`. A caller
that recorded those as fixtures will see them move.
