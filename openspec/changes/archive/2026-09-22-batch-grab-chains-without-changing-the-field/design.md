## Design

The existing blocked walk moves the instruction loop outside the point loop,
but a primitive still decodes every deformer per point. A grab-only primitive
without repetition can transform a block into local coordinates, apply each
grab to that block in authored order, and then evaluate the original primitive
or sampled volume. Use thread-local bounded scratch; do not allocate per point.

Radius clamps, front-gate direction and band are invariant within one grab.
Precompute those exact expressions once. Keep the original divisions, easing,
zero-weight test, displacement arithmetic and final distance/colour sampling.
Do not compose warps, approximate derivatives, replace field normals with lattice
normals, or relax floating-point compiler flags. Mixed chains and repetitions
retain the existing path. Small workloads must be measured before selecting
any minimum batch/chain threshold.

## Verification

Compare object representations against the scalar reference for distances,
colours and gradients across all 33 easing curves, both front-gate modes,
negative/zero/tiny radii and displacement, transforms, hard shapes, volumes,
partial blocks and general-path fallbacks. Include full CPU tests and relevant
sanitizers. Compare preserved main and changed libraries using identical probe
sources and settings, and report attribute-only and end-to-end costs separately.
Host-level performance must not be inferred from a whole-form engine probe.

## Outcome

Compatible batches contain at least four points; smaller calls avoid even the
eligibility scan. The prototype and integrated implementation are validated
against the scalar object representation, including hard-edge normals. See
[validation.md](validation.md) for measured throughput, end-to-end results,
complexity debt and the remaining target-device application check.
