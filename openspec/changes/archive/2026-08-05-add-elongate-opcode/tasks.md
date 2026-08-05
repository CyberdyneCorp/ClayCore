# Tasks: add-elongate-opcode

- [x] 1.1 `cdeform_elongate` opcode: point warp and distance correction in the tape
- [x] 1.2 `scene::Deformer::elongate(h)`; `prim_is_origin_symmetric` predicate
- [x] 1.3 Bounds: per-axis expansion by h
- [x] 1.4 Exactness: preserve when symmetric, downgrade otherwise, without touching Lipschitz
- [x] 1.5 Both bindings, with a negative-extent guard
- [x] 1.6 Tests: tape-vs-kernel, sphere becomes a capsule, exact vs bound, bound containment, chain order, round trip, C-vs-scene
- [x] 1.7 Parity corpus scene; docs; ABI 0.7.0; full verification
