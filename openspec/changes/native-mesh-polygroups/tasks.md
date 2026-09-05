## 1. Decide, before any code

- [x] 1.1 D1 — the unit is the triangle, with a quad-consistency invariant.
      Settled by the tree: `subdivide.cpp` and `dynamic_surface.cpp` both clear
      `Mesh::quads`, so a logical-quad unit does not survive the two operations
      Feature C exists to survive
- [x] 1.2 D2 — a sidecar array rather than a member on `DynamicFace`. A direct
      member costs 80 MB (uint32) at the 20M-face target whether or not the
      feature is used; an empty-when-unused parallel array costs nothing and is
      what `normals`/`colors`/`uvs` already do
- [x] 1.3 D3 — storage implies no constraint; remeshing and sculpt take an
      explicit Preserve/Ignore policy, host default Preserve
- [x] 1.4 D4, D5, D7, D8, D9 — recorded in the proposal
- [ ] 1.5 THE ID WIDTH is deliberately left open: uint16 matches
      `voxel::GroupId` and halves the sidecar; uint32 is the guide's proposal.
      40 MB against 80 MB at 20M faces. Decide with a host

## 2. Not in this change

- [ ] 2.1 Any implementation. This change is a decision record and a spec
      delta; the code is a separate change that starts from these answers
- [ ] 2.2 Whether to build it at all — contingent on hard-surface work or DCC
      round-trip being on the roadmap. If the need is "select a region and
      sculpt it", ABI 0.85.0 already covers it
