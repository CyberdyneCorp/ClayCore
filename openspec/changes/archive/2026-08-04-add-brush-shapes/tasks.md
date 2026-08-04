# Tasks: add-brush-shapes

- [x] 1.1 `BrushShape` enum; set/erase/paint brush take it, defaulting to Cube
- [x] 1.2 Sphere footprint: cells within radius (n-1)/2, subset of the cube
- [x] 1.3 Python: bind `paint_brush` and `paint_mirrored`; `shape=` string arg with a rejecting parser
- [x] 1.4 C++ tests: sphere subset of cube, paint leaves empties, even-size rounding, mirrored paint
- [x] 1.5 pytest: shape argument, paint_brush occupancy invariant, bad shape raises
- [x] 1.6 Example + docs (README capability line, docs/05, examples README)
- [x] 1.7 Full verification: presets, gates, gallery, CI
