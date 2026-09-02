# Design

## Why a budget's slack is worth reporting even when the regression check exists

The obvious objection is that the regression check is the real gate — it
compares against the baseline's `cases[]` at 1.4x, so a loose `budgetMs` should
be cosmetic. It is not, for four reasons:

1. **They go stale together.** `write_baseline` writes budgets and `cases[]`
   from one run, so whatever staleness reaches one reaches the other. For the
   three cases here the regression arm tripped at 123.57 / 112.39 / 6.66 ms
   against measurements of 3.37 / 4.68 / 0.41. Both arms were decorative.
2. **The budget is the only absolute memory.** A re-baseline resets the
   regression reference to whatever the engine does that day, so a 1.35x slip
   per release passes forever and accumulates. The budget is what says "this
   was always too slow".
3. **A case absent from `cases[]` has no regression check at all** — its budget
   is the sole gate.
4. **A `single_observation` case is regression-checked on a statistic whose own
   run-to-run spread is 1.55x against a 1.40x tolerance.** `volume_hpolish` and
   `volume_flatten` are exactly these: their regression arm is the noisy one and
   their budget was meant to be the steady one.

So budget-vs-run slack is a sound proxy for BOTH halves having drifted, and one
ratio detects it.

**Six, not four.** `--update` derives 1.5x, so anything past ~4x is already
suspicious — but a case that is merely FAST in one run should not be named.
`sdf_stamp_bricks` reads 1.50x of its budget in one run and 5.75x in another on
a nearby commit. Six still catches the 9.7x, 17x and 25x this was found by.

**Reported, never failed**, following #338 exactly: that change made the
coverage report honest and changed no verdict. A generous ceiling over
content-varying work is legitimate, so the class is printed and a human decides.

## Why the budgets are derived from a band, not from one run

`--update`'s `round(measured * 1.5, 4)` is right for a case that reproduces to
under 0.5%. It is wrong for one that does not, and two of these three are
`single_observation` cases.

`sdf_flatten` is the clear example: run5 measured 0.4104 ms, near the FLOOR of
its post-fix band (0.4051..0.4913 over 14 valid bracketed records). A budget of
1.5x that draw is 0.6156, only **1.25x above a value the suite has already
produced** — a coin flip, which is the #331 defect this repo has already paid
for twice.

Anchoring on the band top gives 0.7370, still a 9.7x tightening and still 1.5x
above anything observed. The band is taken across valid, BRACKETED records since
the drop landed; unbracketed records are raw and not comparable, which is why
`cut333_runA/B` at 5.20/4.84 ms are excluded from `volume_hpolish`'s band while
its bracketed records span only 3.23..3.46.

## Why the doc is checked rather than generated

Issue #273 offered generation or a checker. The checker wins here because the
table carries prose, footnotes, ZBrush/Nomad mappings and a deliberate ordering
that a generator would flatten. Every row already names its device case, so the
mapping needs no invention.

**Per bundle, because per-case reliability is not uniform.** The
`devicemeasure` cases reproduce to under 0.5% and are held to 1.30x; the gallery
cases move far more on identical code (`move_drags` went 0.149 -> 0.434 ms
between two runs) and are held to 2.10x. A single tolerance would either nag
about the measure bundle or wave the gallery through — which is the issue's own
"note on which numbers are trustworthy".

**The figure is per APPLICATION**, `measuredMs / batch`, because that is what
the column header claims. Without it the check would demand the table publish a
128-dab drag as one dab.

## The one row that must NOT quote the baseline

`sdf_move`. The baseline deliberately holds the pre-regression 0.0791 ms as the
figure a fix must return to; the engine measures ~0.116 ms. The table tells a
host what to expect, and a host expects what the engine does — so the row quotes
0.116 and the checker carries the exemption WITH its reason, the same rule
`check_device_coverage.py` applies to its own. An exemption for a row that has
since vanished is itself a failure, so it cannot rot.
