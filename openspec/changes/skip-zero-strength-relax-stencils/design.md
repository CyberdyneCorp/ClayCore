## Context

The host primes a complete preview by requesting zero-strength relaxation over
the whole layer. Source materialization is necessary with its current preview
architecture. The subsequent smoothing stencil is unnecessary: strength zero
cannot move a stored sample, regardless of its neighborhood or mask.

## Decisions

- Test the clamped strength with equality to zero. Negative values already clamp
  to zero; NaN does not enter the shortcut.
- Skip stencil construction and return `old` before position, mask, or tap reads.
- Retain the existing rewrite/reporting and pass loop. Selected-brick counts,
  dirty bounds, band narrowing per completed pass, and pre-pass cancellation
  remain unchanged. This is deliberately not a new no-op transaction path:
  materialization must still supply the host's complete preview delta.
- Preserve stored sample bits, including negative zero, instead of reconstructing
  an identity through floating-point addition.

The stencil average is a separate helper so the per-sample blend and pass
orchestration remain readable and within the systems complexity target. The
accumulation and division order are unchanged.

## Validation

A counting mask proves the old implementation performs unnecessary sampling.
Check whole-volume and regional calls, clamped-negative and signed-zero strength,
unchanged sample bits, unchanged reporting/band handling, and pre-cancelled work.
Existing nonzero relax, transaction, and native/rendered host tests cover behavior
outside the shortcut. Measure the same live fixture before/after; keep timing
informational and assert structural work/output properties in regressions.
