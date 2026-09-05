## 0. Read first

- [x] 0.1 The voxel regional path (`clay_voxel_add_level_region` and its C++
      side) BEFORE designing the host-facing API, so the two are one idea rather
      than two vocabularies. READ: it keeps the lattice uniform and complete and
      changes only what is STORED, which is the shape this should take
- [x] 0.2 MEASURE where a level's memory goes before building sparsity into the
      wrong place. `DetailField` is already sparse and costs 0.0 MB on an
      unsculpted level; the 121.4 MB of a level-4 hierarchy is topology (10.4),
      evaluated (37.6), chunk index (10.8) and runtime. The sparsity belongs in
      the topology and the derived buffers
- [x] 0.3 Confirm the identity to reuse: faces are patch-major at every level
      and `LevelTopology::face_patch[]` already names the base patch. One
      patch's faces at one level are a contiguous run

## 1. Per-patch depth

- [x] 1.1 Per-patch depth, authoritative. Stored as `MultiresLevel::patch_kept`
      -- one flag per base patch per level, EMPTY on a level that refines every
      patch -- with `patch_max_level(patch)` a walk over it. The per-level set
      is the authoritative form because it is also what serialization replays;
      a `patch_max_level[]` beside it would be a second copy to keep in step
- [x] 1.2 Old surfaces decode as uniform depth everywhere. Stream version 3
      carries the per-level patch sets; a version 1 or 2 stream has none and
      `uniform_depth()` is true, which is exactly what it was
- [x] 1.3 `add_level_for_patches(patches)` takes a PATCH LIST;
      `refine_patches_to_level` sits on top and `examples/74` shows a world-ball
      selection above both

## 2. Balance and transitions

- [x] 2.1 Grading with automatic rings, in ascending patch id. STRICTER than
      2:1 and for a reason the build found: a patch's stencils read every face
      incident to its corners, so refining it needs its whole VERTEX RING one
      level down, not merely a neighbour within one level.
      `refine_patches_to_level` grows level `target - k` by `k` rings;
      `add_level_for_patches` refuses (`PatchNotRefinable`) rather than
      evaluate a border rule at an edge that is not a border. Gated by
      "the same request twice is the same hierarchy", which asks in the
      opposite order
- [x] 2.2 A fine boundary vertex is DERIVED: a regional level evaluates the
      same stencils against the same parent, so its stored vertices are
      bit-identical to the dense level's. Gated numerically -- worst difference
      0.0 over every refined patch, in both the unit test and example 74
- [ ] 2.3 Transition polygons for EXPORT are not built yet, and the audit
      corrected what they are for. A fine patch's corner vertex has taken one
      more subdivision step than its coarse neighbour's, so the two sides are a
      subdivision step apart rather than a hairline apart: a coarse face beside
      a finer patch has to be emitted carrying the finer boundary vertices.
      `mesh_at_level(n)` today exports the faces level `n` HAS. Derived display
      data, never authoritative -- unchanged

## 3. Sparsity

- [x] 3.1 `DetailField` blocks exist only for resident (patch, level) pairs —
      ALREADY TRUE, and measured: an unsculpted level costs 0.0 MB of detail.
      Nothing to build; the gate is that it stays true
- [x] 3.2 `effective_level(patch, level)`, and `patch_resident(level, patch)`
      beside it
- [x] 3.3 Per-dab work follows the resident level: `dirty_children` translates
      to the child's own numbering and DROPS a child the level does not store,
      which is not lost propagation -- a vertex that does not exist here is one
      the host reads at the level below. A cold evaluation of level 4 costs
      9,896 vertices regionally against 110,168 uniformly
- [ ] 3.4 Cross-level neighbours for smooth/relax/normals. NOT DONE: a brush
      at a regional level sees that level's own connectivity, so a stroke that
      runs off the refined region stops at its edge rather than continuing on
      the coarser neighbour. Follows 2.3, which is where the two levels are
      first joined
- [x] 3.5 Propagation reaches resident descendants only and instantiates none
      -- see 3.3

## 4. The runtime, reused

- [x] 4.1 ONE `ChunkTable`, unchanged: a regional level simply has no faces
      for the absent patches. Its reservation now counts the patches the level
      RESIDENTLY holds rather than the cage's, so a level refining sixteen
      patches does not reserve a table for sixteen hundred
- [x] 4.2 `SculptLayerStack` inherits sparsity through `sync_stack_levels`,
      which sizes a layer to the level's vertex count -- the regional one. No
      per-layer topology copies, and none added
- [x] 4.3 `preflight_add_level_for_patches`, through the same `price_level` the
      uniform call now shares. The vertex count is an upper BOUND (one face
      point and two per corner, shared ones counted per face), which is the
      direction a budget has to err in

## 5. Gates

- [x] 5.1 "refining with no detail does not move the surface", bit for bit
- [x] 5.2 "naming every patch IS the uniform level" -- and it is the same code
      rather than an equal one: a request naming every patch takes the dense
      path, and the encoded streams are byte-identical
- [ ] 5.3 Brushes ACROSS a transition, with 2.3 and 3.4. What is gated today
      is the storage side of watertightness -- every stored vertex is the dense
      hierarchy's -- and that a dab at a regional level is an ordinary dab
- [x] 5.4 "the same request twice is the same hierarchy", asked in the
      opposite patch order
- [x] 5.5 "a mixed-depth hierarchy survives a round trip", with detail on it,
      and re-encoding the decoded surface reproduces the stream
- [x] 5.6 THE CLAIM, gated as ratios in the unit tests rather than as a
      benchmark: memory and COUNTED evaluation work, regional against uniform at
      the same level. A wall clock would measure the box; `MultiresEvalStats`
      measures the change. Example 74 reports 8.3x the memory and 11.1x the work
      on a 432-patch cage refined over 26 of them
- [x] 5.7 `clay_multires_add_level_for_patches`,
      `clay_multires_refine_patches_to_level`,
      `clay_multires_preflight_add_level_for_patches`,
      `clay_multires_patch_depth`, `clay_multires_uniform_depth` (ABI 0.85.0);
      pyclay including `topology_at`; `examples/74_regional_multires.py`
- [x] 5.8 No level removal over a region. `remove_highest_level` drops the top
      level whatever it refines, which is the operation that already existed;
      removing a REGION would need a policy for the detail authored there and
      the proposal records why picking one silently is worse
