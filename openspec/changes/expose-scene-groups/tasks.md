# Tasks: expose-scene-groups

- [ ] 1.1 Confirm and write down the semantics the scene model already implements: group op, `Op::None` inline children, transform composition order, bound dilation
- [ ] 1.2 Decide re-parenting: built-once or mutable, and the exact command inverse either way
- [ ] 1.3 C ABI: create a group, add children, enumerate them; node ids as for any other node
- [ ] 1.4 pyclay surface, matching the C ABI so `check_binding_parity` stays clean
- [ ] 1.5 Tests: `(A ∩ B) ∪ C` from a host; a nested group; `Op::None` children apply inline; a carving group with nothing beneath produces nothing; undo of a group edit is exact; `.clayspace` round trip is bit-identical
- [ ] 1.6 Rework `examples/35_hard_surface_helmet.py` to build a plate as a group rather than a layer, and note in the file which technique is which
- [ ] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): no `.clayspace` bump — groups already serialise; runs in parallel with the other two disjoint changes
