# Tasks: add-bend-opcodes

- [x] 1.1 Widen the deformer record; `cdeform_bend_linear` / `cdeform_bend_radial` dispatch
- [x] 1.2 `Deformer` extension floats; type-dispatched serialization keeping old files readable
- [x] 1.3 Factories with degenerate-span guards
- [x] 1.4 Bounds: dilate by the displacement
- [x] 1.5 Exactness: wire the unused `cfi_bend_linear` / `cfi_bend_radial`
- [x] 1.6 Both bindings
- [x] 1.7 Tests: tape-vs-kernel, ramp endpoints, easing reaches the field, bound containment, old-file compatibility, round trip, C-vs-scene
- [x] 1.8 Parity scenes; docs; ABI 0.8.0; full verification
