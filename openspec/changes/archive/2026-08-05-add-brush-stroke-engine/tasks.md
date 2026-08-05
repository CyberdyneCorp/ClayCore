# Tasks: add-brush-stroke-engine

- [x] 1.1 `StrokeSample`, `StrokePreset` (versioned), `Stamp`
- [x] 1.2 `resolve_stroke`: arc-length spacing, single-sample case, steady-stroke smoothing
- [x] 1.3 Pressure curves, taper, rotate-along-stroke, deterministic jitter
- [x] 1.4 Preset serialization: older loads with defaults, newer is refused
- [x] 1.5 Apply to a voxel grid; apply to an SDF layer through the command vocabulary
- [x] 1.6 Mask consumption: drop fully masked stamps, attenuate partial ones
- [x] 1.7 Both bindings
- [x] 1.8 Tests: spacing independence, single sample, pressure, taper, jitter determinism, steady stroke, preset versioning both directions, undo, mask freeze, C-vs-Python
- [x] 1.9 Docs, example, ABI 0.13.0, full verification
