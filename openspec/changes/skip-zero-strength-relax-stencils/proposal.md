## Why

Issue #531's remaining Smooth/Relax pointer-down stall includes a full-field
zero-strength priming update. Three live runs on host e99ace5 / Core v0.113.0
attribute 187–197 ms to this update, versus less than 1 ms opening the transaction
and 5–6 ms copying its preview into the renderer cache. Relax still reads each
stencil and mask before multiplying the average displacement by zero.

## What Changes

Return the stored sample immediately when clamped strength is zero, and avoid
constructing an unused stencil. Preserve priming materialization, geometric
tallies, pass cancellation, band handling, and all nonzero smoothing arithmetic.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `sdf-kernels`: zero-strength relax samples neither the stencil nor the mask.

## Impact

CPU field relaxation and live Smooth/Relax preview priming. No C ABI, file format,
host pin, or preview lifecycle change. Regression and live before/after coverage
must show that the full preview remains available and edits still behave correctly.
