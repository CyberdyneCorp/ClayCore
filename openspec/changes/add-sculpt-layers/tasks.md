# Tasks: add-sculpt-layers

- [x] 1.1 DECIDE what brackets a pass, and whether it follows the undo-group precedent
      — DECIDED: explicit `begin_sculpt_layer` / `end_sculpt_layer`, NOT the undo
      group. An undo group is closed by the host at a gesture boundary; a sculpt
      layer outlives many gestures and is the artist's unit, not the input
      system's. pyclay wraps it as `with grid.sculpt_layer(name):`, which is the
      only form that cannot leave a grid recording when a stroke loop raises.
      Nesting is refused: a cell belongs to one pass.
- [x] 1.2 Record a voxel pass as changed cells plus their previous values
- [x] 1.3 Strength in [0, 1], dithered against the same cell-coordinate hash the falloff brushes use, so partial strength on binary occupancy is reproducible
      — the hash moved to `src/voxel/dither.h`, shared with the falloff brushes
      rather than copied, so the two cannot drift. Each layer holds a fixed seed
      so raising strength ADDS cells instead of reshuffling.
- [x] 1.4 Visibility, reorder, delete, merge down
- [x] 1.5 State the memory budget and what happens when a document exceeds it
      — `sculpt_layer_bytes` / `sculpt_layer_total_bytes` report it and NOTHING
      enforces it. A cap that silently stopped recording would leave the pass on
      the grid and un-dialable, which is a correctness bug wearing a memory
      limit's clothes. A host with a budget merges down (one entry per cell
      instead of two) or ends the layer; both are decisions a user can see.
- [x] 1.6 `.clayspace` persistence, backward-open so an older reader opens the document flattened rather than failing
- [x] 1.7 Both bindings, C ABI additive
- [x] 1.8 Tests: a pass at strength 0 leaves the grid identical to before it; at 1 identical to applying it directly; the same fractional strength gives the same cells on every platform; reordering two passes that touch the same cells is order-dependent and the test pins which order wins; round trip is bit-identical
- [ ] 1.9 DECIDE separately whether SDF sculpt layers are this feature or a weighted group, once expose-scene-groups has landed
      — DEFERRED, as the proposal allowed. A diff of changed cells has no
      counterpart in an edit list; the SDF equivalent is a weighted group, which
      waits on `expose-scene-groups`. This change is voxel-only and says so in
      the docs rather than implying coverage it does not have.
- [x] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): waits for add-multi-resolution; then runs in parallel with add-representation-round-trip
- [x] 0.2 This change takes `.clayspace` minor **10**. (The roadmap said 7, written
      when the current minor was lower; add-multi-resolution and
      add-representation-round-trip landed first and took 8 and 9. The rule the
      roadmap states is what matters — one change, one minor — not the number it
      guessed in advance.) `kClaySpaceMinor` and `kSceneMinor` bumped together;
      a static_assert binds them. The voxel payload is opaque to the container,
      so the bump is a READER SIGNAL: an older build meets an unknown tag and
      falls back to the flattened grid.
