# Tasks: expose-scene-groups

- [x] 1.1 Confirm and write down the semantics the scene model already implements: group op, `Op::None` inline children, transform composition order, bound dilation
- [x] 1.2 Decide re-parenting: built-once or mutable, and the exact command inverse either way — mutable, `MoveNodeCmd` is the reparent and its inverse is the parent and index captured before the move; no new command needed. `SdfContent::move` gained the cycle guard it never had.
- [x] 1.3 C ABI: create a group, add children, enumerate them; node ids as for any other node
- [x] 1.4 pyclay surface, matching the C ABI so `check_binding_parity` stays clean
- [x] 1.5 Tests: `(A ∩ B) ∪ C` from a host; a nested group; `Op::None` children apply inline; a carving group with nothing beneath produces nothing; undo of a group edit is exact; `.clayspace` round trip is bit-identical
- [x] 1.6 `examples/36_groups.py` builds a plate as a group rather than a layer, and `35_hard_surface_helmet.py` says which technique is which and points at it
