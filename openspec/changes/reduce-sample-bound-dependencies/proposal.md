# Reduce sample-bound reduction dependencies

## Why

Issue #531 preview initialization still spends about 18 ms in the engine update. Field materialization measures the largest neighboring sample difference with one serial maximum accumulator. A four-accumulator prototype makes that bounded reduction about 2.8 times faster in isolated probes while retaining float operations and exact results.

## What changes

Use four independent nonnegative float maxima over each sample row, then combine them. Keep every neighboring pair, the row tail, NaN-ignore behavior, infinities and the same final bound. Share the private helper with focused numerical regressions.

## Scope

Private field implementation; no API or storage format change. Run actual-volume exactness and timing comparisons, full CPU checks, sanitizers and combined application checks before drawing conclusions. The 16 ms goal remains open.
