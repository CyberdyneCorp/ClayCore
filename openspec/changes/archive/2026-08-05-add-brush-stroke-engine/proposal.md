# Proposal: one brush stroke engine

## Why

Every tool in the app that draws will need to turn a finger drag into edits,
and each one will otherwise invent its own answer to the same questions: how
far apart to place stamps, what pressure does, whether the stroke tapers,
whether overlapping stamps accumulate. When each tool answers separately, no
two brushes feel alike, and "brush feel" stops being something the engine can
be responsible for.

The engine also cannot currently consume a mask for SDF edits, because there
is nowhere for that to happen — an SDF item is declarative and has no
per-point strength. `add-mask-field` named the stroke engine as where that
lands, and this is it.

## The interface, fixed now

**Stroke samples in, edit items out.** The engine resolves a path into a list
of *stamps*, and a stamp is turned into an ordinary edit — a node appended to a
layer's edit list, or a voxel brush application. Nothing evaluates through a
private path.

That is not a stylistic preference. Emitting ordinary edits is what gives the
brush undo, stroke coalescing, `.clayspace` serialization and picking for free,
because those already work on edits. A brush that evaluated its own stroke
would need every one of them reimplemented, and would give the engine a second
representation of "an edit" to keep consistent with the first.

Resolution is pure: samples and a preset go in, stamps come out, with no
document touched. That makes it testable without a scene and lets a UI preview
a stroke before committing it.

## What Changes

- **`StrokeSample`**: position, pressure, tilt, and a monotone parameter along
  the path. What a stylus reports.
- **`StrokePreset`**: spacing, jitter, pressure curves, rotate-along-stroke,
  taper, steady-stroke smoothing, and accumulation mode. **Versioned from day
  one** — 3DCoat's engine rewrite destroyed user preset libraries, and a schema
  version is the whole cost of not repeating that.
- **`resolve_stroke(samples, preset) -> stamps`**: the pure core.
- **Two consumers**: stamps to voxel brush applications, and stamps to SDF edit
  items appended through the existing command vocabulary.
- **Mask consumption**: a stamp whose position falls in a masked region is
  attenuated or dropped, which is what freeze means for a declarative edit.
- **Determinism**: jitter is a hash of the stamp index and a seed, like the
  brush dither, so the same stroke and preset produce the same edits on every
  platform and through every binding.

## Capabilities

### Added Capabilities

- `brush-engine`: stroke resolution, presets, and the two consumers.

### Modified Capabilities

- `python-bindings` and `c-abi`: the engine reaches both.

## Impact

- New `include/clay/brush/stroke.h` + `src/brush/stroke.cpp`, both bindings,
  tests, docs, an example.
- ABI 0.13.0 — additive.
- Non-goals: image-based alpha stamps (there is no texture pipeline yet, and
  the spec keeps alpha a scalar along the stroke so one can be added without
  redesign), and scripted brushes, which are recorded as deliberately not done.
