## Context

The host primes a complete preview by requesting zero-strength relaxation over
the whole layer. Source materialization is necessary with its current preview
architecture. The subsequent smoothing stencil is unnecessary: strength zero
cannot move a stored sample, regardless of its neighborhood or mask.

## Decisions

- Test the clamped strength with equality to zero. Negative values already clamp
  to zero; NaN does not enter the shortcut.
- Skip stencil construction and return `old` before position, mask, or tap reads.
- Preserve the reporting and pass loop. A zero-strength pass needs no input
  snapshot; a whole-volume zero-strength pass can report the stored set directly
  without rewriting identical samples. Regional calls retain geometric selection.
  Selected-brick counts, dirty bounds, band narrowing per completed pass, and
  pre-pass cancellation remain unchanged. This is deliberately not a new no-op transaction path:
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

## Buffer allocation follow-up

Phase attribution after the stencil shortcut still finds copying and identity
rewriting in the zero-strength pass. Separate one atomic relaxation pass from
its cancellation/iteration loop, and avoid snapshots and whole-field rewriting
when strength is zero. Use the existing allocation-test counter to prevent
sample-buffer copies from returning.

When a completely empty lattice materializes every slot in one request, reserve
its final sample storage once. Do not reserve exact capacity on every local dab:
that would replace amortized vector growth with repeated copying as a gesture
expands. Partial requests and already-populated volumes keep their growth policy.
