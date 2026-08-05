# Tasks: add-wrap-around-opcode

- [x] 1.1 `cdeform_wrap` opcode + dispatch in `ctape_deform_point`
- [x] 1.2 `scene::Deformer::wrap_around(x0, x1)`; serialization and the reference evaluator
- [x] 1.3 Bounds: the swept disc in `deformed_local_bounds`
- [x] 1.4 Exactness: wire the unused `cfi_wrap_around` into `deformer_lipschitz`
- [x] 1.5 Python modifier replacing the raising stub, with a degenerate-interval guard
- [x] 1.6 C ABI enumerator + validation
- [x] 1.7 Tests: tape-vs-kernel, bound conservativeness by sampling, exactness downgrade, chain order, round trip, C-vs-scene
- [x] 1.8 Parity corpus scene so every backend checks it; retire the gate exemption
- [x] 1.9 Docs + example; ABI 0.6.0; full verification
