## 1. The C entry point (carried from finish-regional-multires 5.7 and 6.1)

- [ ] 1.1 A whole-surface mixed export and a per-patch pair, following
      `clay_multires_block_info_get` and `clay_multires_copy_block` in shape.
      The cross-level neighbourhood stays internal and gets no entry point
      unless a host asks for one
- [ ] 1.2 The descriptor rules `clay_multires_copy_block` predates: a descriptor
      starting with `uint32_t struct_size`, grown by appending, a new field's
      zero meaning today's behaviour, caller-owned buffers,
      `CLAY_ERROR_BUFFER_TOO_SMALL` for a short buffer, and a refusal reported
      by name (`MultiresMixedStatus`) rather than success with an empty block
- [ ] 1.3 The header states what the call does NOT promise: no quad list once
      an edge is split, no attributes on a split cage, no residency — a level a
      trim released is brought back to answer it
- [ ] 1.4 The per-vertex level (`Block::vertex_levels`) crosses the boundary
      with the block, empty meaning every vertex is at the requested level

## 2. Bindings and gates

- [ ] 2.1 pyclay and Swift mirrors; `check_binding_parity.py` read from a
      BUILT pyclay, and `check_c_abi.py`'s struct mirror updated
- [ ] 2.2 A C case and a pyclay case that assemble the export on the closed
      torus and assert 0 open edges at display levels 1-3, where the per-patch
      `clay_multires_copy_block` loop leaves 72 / 168 / 264
- [ ] 2.3 The ABI minor moves across the three version lines in the PR that
      adds the entry point
- [ ] 2.4 `docs/09-brush-latency-and-coverage.md` and
      `examples/74_regional_multires.py` stop saying the export is unreachable
