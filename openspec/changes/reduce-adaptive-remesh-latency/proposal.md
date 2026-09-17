## Why

Adaptive Draw on the current mesh fixture reaches 21–28 ms in slower samples.
Remeshing accounts for most of its stamp cost. Reduce unnecessary work while
keeping the same detail settings, deterministic topology and undo semantics.

## What Changes

- Profile remesh stages and retain reproducible evidence.
- Optimize the dominant redundant work without changing operation order or budgets.
- Add regression coverage and compare complete surface output with the baseline.

## Capabilities

No new or modified capability requirements. This is an implementation optimization;
`skip_specs: true` preserves the existing dynamic-topology contract.

## Impact

Adaptive remesh internals and their regression tests; no planned ABI, file format,
dependency or host policy changes. Shared stroke integration follows separately.
