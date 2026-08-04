# Tasks: add-sculpt-brushes

- [x] 1.1 `BrushFalloff`, `BrushParams`, weight function, deterministic cell-hash dither
- [x] 1.2 Brush overloads taking `BrushParams` (set/erase/paint)
- [x] 1.3 Region snapshot helper so verbs read pre-operation state
- [x] 1.4 `sculpt_smooth`, `sculpt_inflate`, `sculpt_flatten`, `sculpt_pinch` in `src/voxel/sculpt.cpp`
- [x] 1.5 Python bindings: verbs plus `falloff`/`strength`/`seed`, with a rejecting parser
- [x] 1.6 C++ tests: constant==hard brush, falloff thins rim, determinism, strength ordering, each verb, order independence
- [x] 1.7 pytest for the same surface
- [x] 1.8 Example + docs
- [x] 1.9 Full verification
