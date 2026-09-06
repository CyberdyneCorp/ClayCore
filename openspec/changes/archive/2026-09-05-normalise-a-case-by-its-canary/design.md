## Context

See `proposal.md`. Three facts decide the shape.

**The engine did not change.** Measured off-device, interleaved, 1.05x at worst.
Anything the design does must therefore make the gate agree with that, not
accommodate a slowdown that is not there.

**The canary already exists and already works.** `make-device-timing-position-independent`
built it, validated it, and deliberately stopped short of acting on it. Nothing
new has to be invented — the denominator is already being recorded, just not at
the resolution a per-case verdict needs.

**The device moves faster than the sampler.** Two pooled cases take it from
x1.07 to x1.50 in thirteen seconds. Any scheme that samples on a timer and lines
cases up against the nearest reading inherits that error.

## Decisions

### Bracket, rather than sample more often

A finer timer would shrink the error without removing it, and would keep the
verdict a function of phase. Bracketing removes the question: the two readings
belong to the case by construction, the way lineage belongs to a tape by
construction rather than by inspection.

The cost is two canary samples per case — ~130 ms each, ~16 s over 61 cases,
against a run already taking ~250 s. That is affordable enough not to trade
against, which is the only reason a timer was ever the right answer.

*Alternative considered — cool down before each budgeted case*, idling until the
canary returns near settled. It is the most rigorous option and it keeps every
budget's current meaning. Rejected on cost and risk: the wait is unbounded in
principle, the suite is already 4-5 minutes, and the gallery bundle already
crashes on heat. It also does not help the cases that generate the heat.

*Alternative considered — report drifted cases as not-comparable.* Smallest
change and fully honest, but it removes the gate from exactly the cases where a
regression could hide, and today that is seven of sixty-one.

### The mean of both ends, not either one

A case that starts cold and ends hot paid something in between. Taking the
opening reading reproduces the interpolation problem in miniature; taking the
closing one charges the case for heat it generated itself. The mean is the
cheapest estimator that is wrong in neither direction, and the spread between
the two ends is retained in the record for anyone who wants to bound it.

### Normalise the comparison, print the measurement

The gate exists to compare code across runs, so what it compares must be
divided by the machine. A user waiting for a brush does not divide by anything,
so the 120 Hz frame-share note stays on the raw figure and the raw figure is
printed beside every normalised one. Two different questions, both answerable
from one record.

### Budgets are derived normalised, and the baseline keeps its canary

A budget derived from a case measured on a throttled device bakes that
throttling in as the engine's cost — which is how `sdf_stamp_cpu` was once
captured at 14.988 ms. Deriving from the normalised figure fixes that, and it
forces the baseline to carry the canary it was normalised against; without it
the gate would compare a normalised run against a raw baseline and silently
reintroduce the error it exists to remove.

## Risks / Trade-offs

**Normalisation could launder a real regression →** it cannot, when the factor
divides both sides: an unchanged machine leaves the ratio untouched. That is a
test, not an argument.

**The canary might not throttle like the verb does →** it is a dense float walk
over an L2-sized buffer and most verbs here are CPU-bound, so both track core
clock. It is an approximation, and the residual is visible: normalising the two
`sdf_move` runs leaves 1.15x rather than 1.00x. That is inside a 1.4x tolerance
and far better than the 1.60x it replaces, but it is not zero and the record
keeps the raw numbers so it can be re-examined.

**Every budget moves once →** unavoidable, and the reason this is its own change
rather than a patch. The re-derivation is a single reviewed commit against a
valid run.

**The gallery bundle stays unnormalised →** its cases set no per-case context at
all, and they are already the least trustworthy half of the suite. They keep
today's behaviour rather than gaining a factor inferred from samples that do not
describe them.

## Migration Plan

Additive on the harness side: a record without brackets is compared raw, so the
tooling change can land before any device run. The baseline is re-derived once,
in its own commit, against a valid run.
