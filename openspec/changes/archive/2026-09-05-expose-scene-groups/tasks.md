# Tasks: expose-scene-groups

- [x] 1.1 Confirm and write down the semantics the scene model already implements: group op, `Op::None` inline children, transform composition order, bound dilation
- [x] 1.2 Decide re-parenting: built-once or mutable, and the exact command inverse either way — mutable, `MoveNodeCmd` is the reparent and its inverse is the parent and index captured before the move; no new command needed. `SdfContent::move` gained the cycle guard it never had.
- [x] 1.3 C ABI: create a group, add children, enumerate them; node ids as for any other node
- [x] 1.4 pyclay surface, matching the C ABI so `check_binding_parity` stays clean
- [x] 1.5 Tests: `(A ∩ B) ∪ C` from a host; a nested group; `Op::None` children apply inline; a carving group with nothing beneath produces nothing; undo of a group edit is exact; `.clayspace` round trip is bit-identical
- [x] 1.6 `examples/37_groups.py` builds a plate as a group rather than a layer, and `35_hard_surface_helmet.py` says which technique is which and points at it
- [ ] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): no `.clayspace` bump — groups already serialise; runs in parallel with the other two disjoint changes

## Found while finishing it

- [x] 2.1 **This list was DUPLICATED** — six entries marked `[x]` with real
      detail, then the same six repeated as `[ ]`, which made a finished change
      read as 6 of 13. Third instance of this pattern in the repo. Deduped.
      Every surviving `[x]` was re-verified against the tree rather than
      trusted: `compile_group` with `Op::None` inline children, `MoveNodeCmd`
      returning its own inverse, the four C entry points, the pyclay mirror,
      and 13 C++ plus 11 Python group cases passing.
- [x] 2.2 **1.6 named a file that does not exist.** The example is real and
      always was, as `examples/37_groups.py` — the name in this list, in
      `docs/05-claycore-library.md` and in `35_hard_surface_helmet.py`'s own
      docstring all say `36_groups.py`, from before `36` became
      `36_mesh_layers.py`. Two shipped documents pointed at nothing, and
      nothing catches a dangling example reference. All three corrected.
      A gate for this belongs in `run_all.py` and is not in this change.
