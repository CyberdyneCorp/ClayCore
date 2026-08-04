# Tasks: add-transition-morphs

- [x] 1.1 Tape: `ccombine_transition_linear`/`radial` modes, variable-width combine params, distance + color mix in `ctape_combine_values`
- [x] 1.2 Scene: `Op::TransitionLinear`/`TransitionRadial` + `Node::transition` parameters; compiler emission
- [x] 1.3 Field info: fold `cfi_transition` with a conservative |d1-d2| bound and the easing curve's measured steepest slope
- [x] 1.4 Bounds: non-local combine modes report infinite influence
- [x] 1.5 Serialization + reference tree evaluator carry transition parameters
- [x] 1.6 Tests: weight endpoints, kernel agreement, step-scale drop and conservative stepping, never-culled property, round trip
- [x] 1.7 Python: Op values + TransitionLinear/TransitionRadial parameter objects, missing-parameter error, tests
- [x] 1.8 Docs + full verification (all presets, python ON/OFF, release checklist)
