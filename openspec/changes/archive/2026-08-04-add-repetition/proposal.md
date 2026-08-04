# Proposal: repetition — grid and radial arrays in documents

## Why

`repeat.h` ships the three repetition operators with their subtle parts already correct: round-based infinite grids, finite grids that clamp the *cell index* rather than the coordinate (clamping the coordinate smears the outermost copies), and O(2) radial arrays. No document can use any of it, so arrays — a staple authoring operation in every SDF modeller — must be hand-duplicated item by item.

Repetition is also where the influence-bound machinery earns its keep a second time: a repeated item occupies many cells, and an *infinite* grid occupies all of them. Getting that wrong silently corrupts culled bricks, which is exactly the failure mode the deformer and transition work already taught us to test for.

## What Changes

- **Repeat modifiers on an item**, alongside the deformer chain: `grid_infinite(spacing)`, `grid_finite(spacing, counts)`, and `radial(count, axis_offset)`.
- **Bounds that tell the truth**:
  - *finite grid* — the item's bound swept across the occupied cell range: still finite, still cullable.
  - *radial array* — the bound swept into an annulus about the axis: finite.
  - *infinite grid* — genuinely unbounded, so it reports **infinite influence** and is never culled, like `intersect` and the transitions.
- **Exactness**: repetition of an exact primitive stays exact only when the primitive plus its rounding and blend influence fits inside its half-cell (01 §2.4). The compiler checks that and downgrades the tracked field info to a bound when it does not, rather than assuming.
- **Python**: `.repeat_grid(spacing, counts=None)` and `.repeat_radial(count, offset)` as chainable primitive modifiers.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: the repetition requirement gains tape reachability and the half-cell exactness condition as an enforced check.
- `scene-model`: influence bounds cover repeated items, including the infinite case.
- `python-bindings`: repetition joins the module's API surface.

### New Capabilities

_None._

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build,commands}.cpp`, `bindings/python/pyclay_module.cpp`, tests, reference evaluator.
- Non-goals: per-element transform overrides (the kernel exposes offsets, but per-instance parameters need a scene-level instancing design), and neighbour-cell padding for items that overflow their half-cell — this change *detects* that case and downgrades exactness rather than evaluating extra cells.
