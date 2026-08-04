# Proposal: a runnable examples gallery with committed renders

## Why

Every capability in this library is verified by tests and described by prose, and neither shows anyone what the thing *looks like*. The 50 pytest cases exercise the Python surface as assertions — correct, but nobody learns the API from them. `docs/05` carries hand-written snippets that have never been executed, so they can drift out of sync with the bindings and no gate notices.

There is a third gap under both: the repository produces no images. For a library whose whole output is geometry, the absence of a visual reference makes review, regression-spotting, and onboarding harder than they need to be — and the ClaySpace app work wants a picture of what each blend mode and deformer actually does.

The bindings already expose everything needed to close this: `raycast_many` returns hit positions and normals, which is enough to shade an image without any renderer dependency, and `mesh().save()` writes models.

## What Changes

- **An `examples/` directory of runnable scripts**, one per feature area, covering the SDF vocabulary (primitives, blends and extended combine modes, deformers, repetition, profile lifts, transitions, strokes) and voxel sculpting (brushes, fills, mirroring, flood select, palettes, greedy meshing, SDF rasterization), plus meshing and file I/O.
- **A dependency-free renderer** in `examples/_render.py`: camera ray generation, `raycast_many` shading, and a pure-stdlib PNG writer (`zlib` + `struct`). No Pillow, no matplotlib — examples run against a bare wheel plus numpy.
- **Committed outputs** under `examples/output/`: a PNG per example and a small set of models, referenced from a README gallery so they are visible on the forge without a local build.
- **A CI job that runs every example** and fails on a non-zero exit, so an API change that breaks an example breaks the build — the gate the docs snippets never had.

## Capabilities

### New Capabilities

- `examples`: the gallery's coverage rule, the no-extra-dependency rule, and the requirement that outputs are reproducible from a checkout.

### Modified Capabilities

- `build-packaging`: CI gains the examples job.
- `picking`: shape bounds account for deformers and repetition. The repetition example surfaced this — `layer_bounds` framed a 5x3x3 grid of spheres as one sphere, because it called `prim_local_bounds` directly while `scene::item_geometry_bound` had always applied both. Fixed by factoring the shared local-shape computation, with a regression test.

## Impact

- New: `examples/` (scripts, shared render helper, committed `output/`), a CI job, README gallery section, `docs/05` pointing at runnable scripts rather than only inline snippets.
- Fixed: `scene::item_local_bounds` factored out of `item_geometry_bound`; `pick::node_shape_bounds` now uses it.
- Renders are CPU raycasts at modest resolution, kept small enough that committing them does not bloat history.
- Non-goals: a real renderer (no shadows, materials, or anti-aliasing beyond simple supersampling), per-commit image diffing (the gate is exit status, not pixel equality — float output across platforms would make pixel comparison flaky), and animation.
