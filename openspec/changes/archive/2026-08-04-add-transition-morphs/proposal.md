# Proposal: transition morphs as tape combine modes

## Why

`transition_linear` and `transition_radial` are the last constructs in `deform.h` with no home in a document. They were left out of `add-tape-deformers` for a real reason: a transition is not a domain warp. It blends **two fields** by a spatially varying weight — `mix(a, b, w(p))` — so it is a binary operator, and the per-item deformer chain has nowhere to put a second operand.

The tape already has the right shape for this: a postfix stack machine whose combine instructions pop two values. Transitions belong there, next to add/subtract/intersect and the extended blends.

## What Changes

- **Two combine modes**: `transition_linear` (weight from the projection onto a segment a→b) and `transition_radial` (weight from XZ radius between r0 and r1), each taking an easing curve. The result mixes both distance *and* color by the same weight.
- **Variable-width combine parameter blocks**: the existing four floats stay; transition modes append their own parameters. Other modes are unaffected and no space is wasted.
- **Non-local by construction, and marked as such.** A transition's weight is non-zero over a half-space (linear) or everything past a radius (radial), so the operator changes the field arbitrarily far from either operand. Items combined this way SHALL report infinite influence — exactly as `intersect` already does — so brick culling never drops them. This is the blend-locality principle doing its job: only rigid blends are local, and the library says so rather than silently corrupting culled bricks.
- **Field classification**: a lerp of two distance fields is not a distance. The compiler folds `cfi_transition` with a conservative bound on |d₁ − d₂| (the diameter of the union of both operands' bounds) and the weight's Lipschitz factor, measured from the chosen easing curve's steepest slope.
- **Python**: `clay.Op.TRANSITION_LINEAR` / `TRANSITION_RADIAL` with a `transition=clay.TransitionLinear(a, b, ease=…)` / `clay.TransitionRadial(r0, r1, ease=…)` argument.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: transitions become reachable from a document, completing the deformer requirement.
- `scene-model`: influence bounds gain the infinite case for non-local combine modes.
- `python-bindings`: transitions stop being the documented exception.

### New Capabilities

_None._

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build,commands}.cpp`, `bindings/python/pyclay_module.cpp`, tests, and the reference tree evaluator.
- Backends inherit the modes through the shared tape header; the parity suite is the check.
- Documents using transitions are readable by this version onward; older documents are unaffected (the reader defaults the new parameters).
- Non-goals: `wrap_around` (a point warp whose inverse-mapping semantics need their own design pass) stays header-only, and transitions remain unavailable as *group* operators — they combine the accumulated field with one item, matching every other combine mode.
